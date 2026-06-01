#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <optional>
#include <queue>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "httplib.h"
#include "json.hpp"

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__linux__)
#include <unistd.h>
#endif

using json = nlohmann::json;
namespace fs = std::filesystem;

struct ServerProfile {
    std::string name = "default";
    std::string host = "0.0.0.0";
    int port = 8085;
    int threads = 2;
};

struct Config {
    std::string active_server = "default";
    ServerProfile server;
    std::vector<std::string> video_folders = {"./videos"};
    std::vector<std::string> allowed_extensions = {".mp4", ".mkv", ".webm", ".mov"};
    std::string metadata_dir = "./metadata";
    std::string tmdb_api_key;
    bool fetch_tmdb_posters = true;
    bool recursive_scan = true;
};

struct Video {
    std::string id;
    std::string filename;
    std::string path;
    uint64_t size_bytes = 0;
    std::string size_human;
    std::string format;
    std::string mime_type;
    std::string poster_path;
    std::string title;
    std::string year;
    std::string plot;
    std::string tmdb_id;
};

Config g_config;
std::vector<Video> g_videos;
std::mutex g_videos_mutex;
std::atomic<int> g_active_streams{0};
std::atomic<int> g_active_requests{0};

std::queue<std::string> g_poster_queue;
std::set<std::string> g_queued_posters;
std::mutex g_poster_mutex;
std::condition_variable g_poster_cv;
std::atomic<bool> g_worker_running{true};

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return value;
}

std::string human_size(uint64_t bytes) {
    double size = static_cast<double>(bytes);
    int unit = 0;
    const char* units[] = {"B", "KB", "MB", "GB", "TB"};
    while (size >= 1024.0 && unit < 4) {
        size /= 1024.0;
        ++unit;
    }

    std::ostringstream out;
    out << std::fixed << std::setprecision(2) << size << " " << units[unit];
    return out.str();
}

std::string mime_type_for_extension(const std::string& ext) {
    const std::string lower = lower_copy(ext);
    if (lower == ".mp4") return "video/mp4";
    if (lower == ".m4v") return "video/mp4";
    if (lower == ".webm") return "video/webm";
    if (lower == ".mkv") return "video/x-matroska";
    if (lower == ".mov") return "video/quicktime";
    if (lower == ".avi") return "video/x-msvideo";
    return "application/octet-stream";
}

std::string image_mime_type(const std::string& ext) {
    const std::string lower = lower_copy(ext);
    if (lower == ".png") return "image/png";
    if (lower == ".webp") return "image/webp";
    return "image/jpeg";
}

std::string stable_id(const std::string& value) {
    const size_t hash = std::hash<std::string>{}(value);
    std::ostringstream out;
    out << std::hex << std::setw(16) << std::setfill('0') << hash;
    return out.str();
}

double ram_usage_mb() {
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        return static_cast<double>(pmc.WorkingSetSize) / (1024.0 * 1024.0);
    }
    return 0.0;
#elif defined(__linux__)
    std::ifstream status("/proc/self/status");
    std::string line;
    while (std::getline(status, line)) {
        if (line.rfind("VmRSS:", 0) == 0) {
            const auto start = line.find_first_of("0123456789");
            if (start == std::string::npos) return 0.0;
            const auto end = line.find_first_not_of("0123456789", start);
            return std::stod(line.substr(start, end - start)) / 1024.0;
        }
    }
    return 0.0;
#else
    return 0.0;
#endif
}

std::string first_xml_value(const std::string& xml, const std::string& tag) {
    const std::regex rx("<" + tag + R"((?:\s[^>]*)?>([\s\S]*?)</)" + tag + ">",
                        std::regex_constants::icase);
    std::smatch match;
    if (std::regex_search(xml, match, rx) && match.size() > 1) {
        return match[1].str();
    }
    return "";
}

void parse_nfo(const fs::path& nfo_path, Video& video) {
    std::ifstream file(nfo_path);
    if (!file.is_open()) return;

    std::ostringstream buffer;
    buffer << file.rdbuf();
    const std::string xml = buffer.str();

    const std::string title = first_xml_value(xml, "title");
    const std::string year = first_xml_value(xml, "year");
    const std::string plot = first_xml_value(xml, "plot");
    const std::string tmdb_id = first_xml_value(xml, "tmdbid");

    if (!title.empty()) video.title = title;
    if (!year.empty()) video.year = year;
    if (!plot.empty()) video.plot = plot;
    if (!tmdb_id.empty()) video.tmdb_id = tmdb_id;
}

void parse_filename(const std::string& filename, Video& video) {
    const fs::path path(filename);
    const std::string stem = path.stem().string();

    std::smatch match;
    const std::regex tmdb_rx(R"(\[tmdbid-(\d+)\])", std::regex_constants::icase);
    if (std::regex_search(stem, match, tmdb_rx) && match.size() > 1 && video.tmdb_id.empty()) {
        video.tmdb_id = match[1].str();
    }

    const std::regex bracket_year_rx(R"((.+?)\s*[\(\[](\d{4})[\)\]])");
    if (std::regex_search(stem, match, bracket_year_rx) && match.size() > 2) {
        if (video.title.empty()) video.title = match[1].str();
        if (video.year.empty()) video.year = match[2].str();
        return;
    }

    const std::regex dot_year_rx(R"((.+?)\.(\d{4})(?:\.|$))");
    if (std::regex_search(stem, match, dot_year_rx) && match.size() > 2) {
        std::string title = match[1].str();
        std::replace(title.begin(), title.end(), '.', ' ');
        if (video.title.empty()) video.title = title;
        if (video.year.empty()) video.year = match[2].str();
        return;
    }

    if (video.title.empty()) video.title = stem;
}

std::string find_local_poster(const fs::path& video_path) {
    const fs::path dir = video_path.parent_path();
    const std::string stem = video_path.stem().string();
    const std::vector<std::string> image_exts = {".jpg", ".jpeg", ".png", ".webp",
                                                 ".JPG", ".JPEG", ".PNG", ".WEBP"};

    for (const auto& ext : image_exts) {
        fs::path candidate = dir / (stem + ext);
        if (fs::is_regular_file(candidate)) return candidate.string();
    }

    const std::vector<std::string> names = {"poster", "folder", "cover", "Poster", "Folder", "Cover"};
    for (const auto& name : names) {
        for (const auto& ext : image_exts) {
            fs::path candidate = dir / (name + ext);
            if (fs::is_regular_file(candidate)) return candidate.string();
        }
    }

    return "";
}

bool extension_allowed(const std::string& ext, const Config& cfg) {
    const std::string lower = lower_copy(ext);
    return std::any_of(cfg.allowed_extensions.begin(), cfg.allowed_extensions.end(),
                       [&](const std::string& allowed) { return lower_copy(allowed) == lower; });
}

void enqueue_poster_fetch(const std::string& video_id) {
    if (!g_config.fetch_tmdb_posters || g_config.tmdb_api_key.empty()) return;

    std::lock_guard<std::mutex> lock(g_poster_mutex);
    if (g_queued_posters.insert(video_id).second) {
        g_poster_queue.push(video_id);
        g_poster_cv.notify_one();
    }
}

std::vector<Video> scan_videos(const Config& cfg) {
    std::vector<Video> videos;
    const fs::path poster_dir = fs::path(cfg.metadata_dir) / "posters";

    for (const auto& folder : cfg.video_folders) {
        const fs::path root(folder);
        if (!fs::is_directory(root)) {
            std::cerr << "[scan] Skipping missing folder: " << root.string() << "\n";
            continue;
        }

        auto handle_path = [&](const fs::directory_entry& entry) {
            if (!entry.is_regular_file()) return;

            const fs::path path = entry.path();
            if (!extension_allowed(path.extension().string(), cfg)) return;

            Video video;
            video.path = fs::absolute(path).lexically_normal().string();
            video.filename = path.filename().string();
            video.id = stable_id(video.path);
            video.size_bytes = static_cast<uint64_t>(entry.file_size());
            video.size_human = human_size(video.size_bytes);
            video.format = lower_copy(path.extension().string());
            video.mime_type = mime_type_for_extension(video.format);

            parse_filename(video.filename, video);

            fs::path nfo_path = path;
            nfo_path.replace_extension(".nfo");
            if (fs::is_regular_file(nfo_path)) parse_nfo(nfo_path, video);

            video.poster_path = find_local_poster(path);
            if (video.poster_path.empty()) {
                fs::path cached = poster_dir / (video.id + ".jpg");
                if (fs::is_regular_file(cached) && fs::file_size(cached) > 0) {
                    video.poster_path = cached.string();
                }
            }

            videos.push_back(std::move(video));
        };

        try {
            if (cfg.recursive_scan) {
                for (const auto& entry : fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied)) {
                    handle_path(entry);
                }
            } else {
                for (const auto& entry : fs::directory_iterator(root, fs::directory_options::skip_permission_denied)) {
                    handle_path(entry);
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[scan] Failed scanning " << root.string() << ": " << e.what() << "\n";
        }
    }

    std::sort(videos.begin(), videos.end(), [](const Video& a, const Video& b) {
        return lower_copy(a.title) < lower_copy(b.title);
    });

    return videos;
}

void refresh_cache() {
    auto videos = scan_videos(g_config);
    {
        std::lock_guard<std::mutex> lock(g_videos_mutex);
        g_videos = std::move(videos);
    }

    std::vector<std::string> missing_posters;
    {
        std::lock_guard<std::mutex> lock(g_videos_mutex);
        for (const auto& video : g_videos) {
            if (video.poster_path.empty()) missing_posters.push_back(video.id);
        }
    }

    for (const auto& id : missing_posters) enqueue_poster_fetch(id);
    std::cout << "[scan] Indexed " << missing_posters.size() << " video(s) missing posters.\n";
}

std::optional<Video> find_video(const std::string& id) {
    std::lock_guard<std::mutex> lock(g_videos_mutex);
    auto it = std::find_if(g_videos.begin(), g_videos.end(), [&](const Video& video) {
        return video.id == id;
    });
    if (it == g_videos.end()) return std::nullopt;
    return *it;
}

json video_to_json(const Video& video) {
    return {
        {"id", video.id},
        {"filename", video.filename},
        {"path", video.path},
        {"size_bytes", video.size_bytes},
        {"size_human", video.size_human},
        {"format", video.format},
        {"mime_type", video.mime_type},
        {"title", video.title},
        {"year", video.year},
        {"plot", video.plot},
        {"tmdb_id", video.tmdb_id},
        {"has_poster", !video.poster_path.empty()},
        {"poster_url", video.poster_path.empty() ? "" : ("/api/videos/" + video.id + "/poster")},
        {"stream_url", "/api/stream/" + video.id}
    };
}

std::optional<json> tmdb_get_json(const std::string& path, const httplib::Params& params) {
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    (void)path;
    (void)params;
    std::cerr << "[tmdb] This build lacks HTTPS support; poster fetching is disabled.\n";
    return std::nullopt;
#else
    httplib::Client client("https://api.themoviedb.org");
    client.set_connection_timeout(5);
    client.set_read_timeout(10);
    client.set_follow_location(true);

    httplib::Headers headers = {{"User-Agent", "playhub_server/1.0"}};
    auto result = client.Get(path, params, headers);
    if (!result || result->status != 200) return std::nullopt;

    try {
        return json::parse(result->body);
    } catch (...) {
        return std::nullopt;
    }
#endif
}

std::optional<std::string> find_tmdb_poster_path(const Video& video) {
    httplib::Params params = {{"api_key", g_config.tmdb_api_key}};

    if (!video.tmdb_id.empty()) {
        auto movie = tmdb_get_json("/3/movie/" + video.tmdb_id, params);
        if (movie && movie->contains("poster_path") && !(*movie)["poster_path"].is_null()) {
            return (*movie)["poster_path"].get<std::string>();
        }
    }

    if (video.title.empty()) return std::nullopt;

    params.emplace("query", video.title);
    if (!video.year.empty()) params.emplace("year", video.year);

    auto search = tmdb_get_json("/3/search/movie", params);
    if (!search || !search->contains("results") || !(*search)["results"].is_array()) {
        return std::nullopt;
    }

    for (const auto& result : (*search)["results"]) {
        if (result.contains("poster_path") && !result["poster_path"].is_null()) {
            return result["poster_path"].get<std::string>();
        }
    }

    return std::nullopt;
}

bool download_tmdb_poster(const std::string& poster_path, const fs::path& dest) {
#ifndef CPPHTTPLIB_OPENSSL_SUPPORT
    (void)poster_path;
    (void)dest;
    return false;
#else
    httplib::Client client("https://image.tmdb.org");
    client.set_connection_timeout(5);
    client.set_read_timeout(20);
    client.set_follow_location(true);

    std::ofstream file(dest, std::ios::binary);
    if (!file.is_open()) return false;

    httplib::Headers headers = {{"User-Agent", "playhub_server/1.0"}};
    auto result = client.Get("/t/p/w500" + poster_path, headers,
        [&](const char* data, size_t length) {
            file.write(data, static_cast<std::streamsize>(length));
            return file.good();
        });

    file.close();
    if (!result || result->status != 200 || !fs::is_regular_file(dest) || fs::file_size(dest) == 0) {
        std::error_code ec;
        fs::remove(dest, ec);
        return false;
    }

    return true;
#endif
}

void poster_worker() {
    while (g_worker_running) {
        std::string id;
        {
            std::unique_lock<std::mutex> lock(g_poster_mutex);
            g_poster_cv.wait(lock, [] { return !g_poster_queue.empty() || !g_worker_running; });
            if (!g_worker_running && g_poster_queue.empty()) break;
            id = g_poster_queue.front();
            g_poster_queue.pop();
        }

        auto video = find_video(id);
        if (!video || !video->poster_path.empty()) continue;

        const auto poster_path = find_tmdb_poster_path(*video);
        if (!poster_path) continue;

        const fs::path dest = fs::path(g_config.metadata_dir) / "posters" / (video->id + ".jpg");
        if (!download_tmdb_poster(*poster_path, dest)) continue;

        std::lock_guard<std::mutex> lock(g_videos_mutex);
        for (auto& cached : g_videos) {
            if (cached.id == video->id) {
                cached.poster_path = dest.string();
                break;
            }
        }

        std::cout << "[tmdb] Cached poster for " << video->filename << "\n";
    }
}

ServerProfile parse_server_profile(const json& item) {
    ServerProfile profile;
    profile.name = item.value("name", profile.name);
    profile.host = item.value("host", profile.host);
    profile.port = item.value("port", profile.port);
    profile.threads = std::max(1, std::min(8, item.value("threads", profile.threads)));
    return profile;
}

Config load_config(const std::string& path) {
    Config cfg;
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "[config] Missing " << path << "; using defaults.\n";
        return cfg;
    }

    json data;
    file >> data;

    cfg.active_server = data.value("active_server", cfg.active_server);

    if (data.contains("servers") && data["servers"].is_array()) {
        for (const auto& item : data["servers"]) {
            ServerProfile profile = parse_server_profile(item);
            if (profile.name == cfg.active_server) {
                cfg.server = profile;
                break;
            }
        }
    } else if (data.contains("server") && data["server"].is_object()) {
        cfg.server = parse_server_profile(data["server"]);
    }

    if (data.contains("library") && data["library"].is_object()) {
        const auto& library = data["library"];
        cfg.video_folders = library.value("video_folders", cfg.video_folders);
        cfg.allowed_extensions = library.value("allowed_extensions", cfg.allowed_extensions);
        cfg.recursive_scan = library.value("recursive_scan", cfg.recursive_scan);
    }

    if (data.contains("metadata") && data["metadata"].is_object()) {
        const auto& metadata = data["metadata"];
        cfg.metadata_dir = metadata.value("metadata_dir", cfg.metadata_dir);
        cfg.tmdb_api_key = metadata.value("tmdb_api_key", cfg.tmdb_api_key);
        cfg.fetch_tmdb_posters = metadata.value("fetch_tmdb_posters", cfg.fetch_tmdb_posters);
    }

    return cfg;
}

json config_to_json(const Config& cfg) {
    return {
        {"active_server", cfg.active_server},
        {"server", {
            {"name", cfg.server.name},
            {"host", cfg.server.host},
            {"port", cfg.server.port},
            {"threads", cfg.server.threads}
        }},
        {"library", {
            {"video_folders", cfg.video_folders},
            {"allowed_extensions", cfg.allowed_extensions},
            {"recursive_scan", cfg.recursive_scan}
        }},
        {"metadata", {
            {"metadata_dir", cfg.metadata_dir},
            {"tmdb_api_key_configured", !cfg.tmdb_api_key.empty()},
            {"fetch_tmdb_posters", cfg.fetch_tmdb_posters}
        }}
    };
}

void set_json(httplib::Response& res, const json& body, int status = 200) {
    res.status = status;
    res.set_content(body.dump(), "application/json");
}

void set_cors(httplib::Response& res) {
    res.set_header("Access-Control-Allow-Origin", "*");
    res.set_header("Access-Control-Allow-Methods", "GET, POST, OPTIONS");
    res.set_header("Access-Control-Allow-Headers", "Content-Type, Range");
    res.set_header("Access-Control-Expose-Headers", "Accept-Ranges, Content-Length, Content-Range");
}

int main(int argc, char** argv) {
    const std::string config_path = argc > 1 ? argv[1] : "config.json";
    try {
        g_config = load_config(config_path);
        fs::create_directories(fs::path(g_config.metadata_dir) / "posters");
    } catch (const std::exception& e) {
        std::cerr << "[startup] Failed to initialize: " << e.what() << "\n";
        return 1;
    }

    refresh_cache();
    std::thread worker(poster_worker);

    httplib::Server server;
    server.new_task_queue = [] {
        return new httplib::ThreadPool(g_config.server.threads);
    };

    server.set_pre_routing_handler([](const httplib::Request&, httplib::Response& res) {
        ++g_active_requests;
        set_cors(res);
        return httplib::Server::HandlerResponse::Unhandled;
    });

    server.set_post_routing_handler([](const httplib::Request&, httplib::Response&) {
        --g_active_requests;
    });

    server.Options(".*", [](const httplib::Request&, httplib::Response& res) {
        set_cors(res);
        res.status = 204;
    });

    server.Get("/api/health", [](const httplib::Request&, httplib::Response& res) {
        set_json(res, {{"status", "ok"}});
    });

    server.Get("/api/config", [](const httplib::Request&, httplib::Response& res) {
        set_json(res, config_to_json(g_config));
    });

    server.Get("/api/stats", [](const httplib::Request&, httplib::Response& res) {
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(g_videos_mutex);
            count = g_videos.size();
        }
        set_json(res, {
            {"videos", count},
            {"active_streams", g_active_streams.load()},
            {"active_requests", g_active_requests.load()},
            {"ram_mb", ram_usage_mb()},
            {"threads", g_config.server.threads}
        });
    });

    server.Get("/api/videos", [](const httplib::Request&, httplib::Response& res) {
        json items = json::array();
        {
            std::lock_guard<std::mutex> lock(g_videos_mutex);
            for (const auto& video : g_videos) items.push_back(video_to_json(video));
        }
        set_json(res, items);
    });

    server.Get("/api/videos/:id", [](const httplib::Request& req, httplib::Response& res) {
        auto video = find_video(req.path_params.at("id"));
        if (!video) {
            set_json(res, {{"error", "video not found"}}, 404);
            return;
        }
        set_json(res, video_to_json(*video));
    });

    server.Post("/api/refresh", [](const httplib::Request&, httplib::Response& res) {
        refresh_cache();
        size_t count = 0;
        {
            std::lock_guard<std::mutex> lock(g_videos_mutex);
            count = g_videos.size();
        }
        set_json(res, {{"status", "ok"}, {"videos", count}});
    });

    server.Get("/api/videos/:id/poster", [](const httplib::Request& req, httplib::Response& res) {
        auto video = find_video(req.path_params.at("id"));
        if (!video || video->poster_path.empty() || !fs::is_regular_file(video->poster_path)) {
            res.status = 404;
            res.set_content("Poster not found", "text/plain");
            return;
        }

        std::ifstream file(video->poster_path, std::ios::binary);
        std::ostringstream buffer;
        buffer << file.rdbuf();
        res.set_content(buffer.str(), image_mime_type(fs::path(video->poster_path).extension().string()));
    });

    server.Get("/api/stream/:id", [](const httplib::Request& req, httplib::Response& res) {
        auto video = find_video(req.path_params.at("id"));
        if (!video) {
            res.status = 404;
            res.set_content("Video not found", "text/plain");
            return;
        }

        auto file = std::make_shared<std::ifstream>(video->path, std::ios::binary);
        if (!file->is_open()) {
            res.status = 500;
            res.set_content("Could not open video file", "text/plain");
            return;
        }

        res.set_header("Accept-Ranges", "bytes");
        ++g_active_streams;

        res.set_content_provider(
            video->size_bytes,
            video->mime_type,
            [file](size_t offset, size_t length, httplib::DataSink& sink) {
                if (!file->good()) return false;

                file->seekg(static_cast<std::streamoff>(offset), std::ios::beg);
                char buffer[64 * 1024];
                const size_t to_read = std::min(sizeof(buffer), length);
                file->read(buffer, static_cast<std::streamsize>(to_read));
                const std::streamsize read = file->gcount();

                if (read <= 0) return false;
                sink.write(buffer, static_cast<size_t>(read));
                return true;
            },
            [](bool) {
                --g_active_streams;
            }
        );
    });

    std::cout << "playhub_server listening on http://" << g_config.server.host
              << ":" << g_config.server.port << "\n";
    std::cout << "threads=" << g_config.server.threads
              << " folders=" << g_config.video_folders.size() << "\n";

    const bool ok = server.listen(g_config.server.host.c_str(), g_config.server.port);

    g_worker_running = false;
    g_poster_cv.notify_all();
    if (worker.joinable()) worker.join();

    if (!ok) {
        std::cerr << "[server] Listen failed\n";
        return 1;
    }

    return 0;
}
