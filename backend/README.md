# Backend API
Requires FFTW and libsndfile.

macOS:
```bash
brew install fftw libsndfile
```

Ubuntu/Debian:
```bash
sudo apt-get update
sudo apt-get install -y libfftw3-dev libsndfile1-dev
```

Build & run:
```bash
cd backend
make
./server
```

Endpoints:
- `GET /songs`
- `POST /upload?name=My%20Song.wav` — body is raw WAV bytes
- `POST /recognize` — body is raw WAV bytes (5–8 seconds works well)
CORS is enabled for localhost.
