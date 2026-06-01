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
cp config.example.json config.json
./playhub_server config.json
```

## GitHub Actions Deployment

The repository includes `.github/workflows/deploy-playhub-server.yml`.
It builds `backend/playhub_server/playhub_server` on GitHub's Ubuntu runner, copies only that binary to the VPS, and restarts the systemd service.

Required GitHub repository secrets:

- `ORACLEVM_HOST`: VPS IP or DNS name
- `ORACLEVM_USER`: SSH username
- `ORACLEVM_SSH_KEY`: private deploy key with SSH access to the VPS
- `ORACLEVM_PORT`: optional, defaults to `22`

Optional GitHub repository variables:

- `PLAYHUB_REMOTE_DIR`: defaults to `/opt/playhub_server`
- `PLAYHUB_SERVICE_NAME`: defaults to `playhub_server`

One-time VPS layout:

```sh
sudo mkdir -p /opt/playhub_server
sudo chown root:root /opt/playhub_server
```

Example systemd service:

```ini
[Unit]
Description=Play Hub C++ streaming server
After=network.target

[Service]
Type=simple
WorkingDirectory=/opt/playhub_server
ExecStart=/opt/playhub_server/playhub_server /opt/playhub_server/config.json
Restart=on-failure
RestartSec=3

[Install]
WantedBy=multi-user.target
```

Keep `config.json` and `metadata/` on the VPS under `/opt/playhub_server`.
The workflow intentionally deploys only the compiled `playhub_server` binary.
Do not commit the real `config.json`; use `config.example.json` as the repository-safe template.

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
