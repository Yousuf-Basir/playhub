# playhub_server

Minimal C++ video streaming backend for Play Hub.

Included:

- JSON-only HTTP API
- recursive video library scan
- byte-range video streaming from the original file
- local `.nfo` metadata parsing
- local poster lookup beside the video file
- optional TMDB poster caching

Excluded:

- admin dashboard web UI
- on-the-fly video/audio conversion
- embedded subtitle/audio inspection
- mutable folder/config admin endpoints

## Build

Linux VPS:

```sh
sudo apt-get install -y build-essential libssl-dev
make
```

Run:

```sh
./playhub_server config.json
```

## API

- `GET /api/health`
- `GET /api/config`
- `GET /api/stats`
- `GET /api/videos`
- `GET /api/videos/{id}`
- `GET /api/videos/{id}/poster`
- `GET /api/stream/{id}`
- `POST /api/refresh`

`/api/stream/{id}` serves the original file with byte-range support. Browser/device support depends on whether the client can play that original container and codec.
