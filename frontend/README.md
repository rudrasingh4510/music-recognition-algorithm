# Frontend
Open `index.html` in your browser. It talks to `http://localhost:5001`.

Panels:
1. **Upload Song to DB** — choose a WAV and upload. Backend fingerprints and stores in-memory.
2. **DB Contents** — view songs loaded into DB.
3. **Identify a Clip** — either upload a WAV clip or record N seconds in the browser (WAV encoder included). The backend returns the best match + score.
