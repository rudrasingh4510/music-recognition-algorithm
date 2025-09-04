const API = "https://musicrec.mooo.com";

function $(id){ return document.getElementById(id); }

// Helper for status updates
function setStatus(element, message, type) {
  const isCard = element.id === 'idResult' || element.id === 'songStatus';
  const contentContainer = isCard ? element.querySelector('.result-content') : element;
  
  if (!contentContainer) { return; }

  contentContainer.innerHTML = ''; 
  element.className = 'status-bar';

  if (isCard) {
      const closeBtn = element.querySelector('.close-btn');
      if (closeBtn) closeBtn.style.display = 'none';
      if(element.id === 'idResult' || element.id === 'songStatus') element.classList.add('result-card');
  }

  if (!message) {
      element.classList.remove('visible');
      return;
  }

  element.classList.add('visible');
  
  if (type === 'loading') {
    element.classList.add('loading');
    contentContainer.innerHTML = `<div class="loader"></div><span>${message}</span>`;
  } else {
    contentContainer.innerHTML = message;
    if (type === 'error') element.classList.add('error');
    if (type === 'success') element.classList.add('success');

    if (isCard) {
        const closeBtn = element.querySelector('.close-btn');
        if (closeBtn) closeBtn.style.display = 'flex';
    }
  }
}

// --- Dropdown Logic ---
const dbToggleBtn = $("dbToggleBtn");
const dbDropdownContent = $("dbDropdownContent");

dbToggleBtn.onclick = () => {
  dbDropdownContent.classList.toggle("show");
};

window.onclick = (event) => {
  if (!dbToggleBtn.contains(event.target) && !dbDropdownContent.contains(event.target)) {
    if (dbDropdownContent.classList.contains('show')) {
      dbDropdownContent.classList.remove('show');
    }
  }
};


// 1) Add song from Local File -> /upload
$("btnUploadSong").onclick = async () => {
  const name = $("songName").value.trim();
  const file = $("songFile").files[0];

  if (!name) {
    return alert("A song label is required.");
  }
  if (!file) {
    return alert("Please choose a WAV file.");
  }

  const statusBox = $("songStatus");
  setStatus(statusBox, "Uploading and fingerprinting...", 'loading');
  $("btnUploadSong").disabled = true;

  try {
    const res = await fetch(`${API}/upload?name=${encodeURIComponent(name)}`, {
      method: "POST",
      headers: { "Content-Type": "audio/wav" },
      body: await file.arrayBuffer()
    });
    
    if(res.ok) {
        const data = await res.json();
        const successHtml = `
        <p class="added-label">Added:</p>
        <p class="success-song-name">${data.name}</p>
        `;
        setStatus(statusBox, successHtml, 'success');
        refreshSongs();
        $("songName").value = "";
        $("songFile").value = "";
        document.querySelector('label[for="songFile"]').innerHTML = `<i class="fa-solid fa-file-audio"></i> Choose WAV File`;
    } else {
        const errorData = await res.json();
        setStatus(statusBox, `Error: ${errorData.error || 'Unknown error'}`, 'error');
    }
  } catch (e) {
    setStatus(statusBox, "Error: Failed to connect to server.", 'error');
  } finally {
    $("btnUploadSong").disabled = false;
  }
};

// Update file input label on change
$("songFile").addEventListener('change', () => {
    const fileInput = $("songFile");
    const label = document.querySelector('label[for="songFile"]');
    if (fileInput.files.length > 0) {
        label.innerHTML = `<i class="fa-solid fa-check"></i> ${fileInput.files[0].name}`;
    } else {
        label.innerHTML = `<i class="fa-solid fa-file-audio"></i> Choose WAV File`;
    }
});


// 2) Refresh DB list -> /songs (now in dropdown)
async function refreshSongs(){
  try {
    const res = await fetch(`${API}/songs`);
    const data = await res.json();
    const songList = $("songs");
    songList.innerHTML = "";
    if (data.songs.length === 0) {
      songList.innerHTML = `<li>Database is empty.</li>`;
      return;
    }
    data.songs.forEach((s, index) => {
      const li = document.createElement("li");
      const text = `${index + 1}. ${s.name}`;
      if (s.url) {
          const a = document.createElement("a");
          a.href = s.url;
          a.textContent = text;
          a.target = "_blank";
          li.appendChild(a);
      } else { li.textContent = text; }
      songList.appendChild(li);
    });
  } catch (e) {
  $("songs").innerHTML = `<li>Failed to load songs.</li>`;
  const warning = $("networkWarning");
  if (warning) warning.style.display = "block";
}
}
refreshSongs();


// 3) Record N seconds as WAV in-browser, then recognize
let micStream = null;
const btnRecord = $("btnRecord");
$("btnRecord").onclick = async () => {
  const secs = 5;
  const resultBox = $("idResult");
  let countdownInterval;

  try {
    btnRecord.disabled = true; // 🔒 disable mic button
    btnRecord.classList.add('recording');
    setStatus(resultBox, "", null);

    if (!micStream) {
      setStatus(resultBox, "Waiting for microphone permission...", 'loading');
      micStream = await navigator.mediaDevices.getUserMedia({ audio: true });
    }

    let timeLeft = secs;
    setStatus(resultBox, `Recording... (${timeLeft}s)`, 'loading');
    countdownInterval = setInterval(() => {
      timeLeft--;
      if (timeLeft >= 0) {
        setStatus(resultBox, `Recording... (${timeLeft}s)`, 'loading');
      } else {
        clearInterval(countdownInterval);
      }
    }, 1000);

    const blob = await recordAndEncodeWav(micStream, secs);
    clearInterval(countdownInterval);

    setStatus(resultBox, "Identifying...", 'loading');
    const res = await fetch(`${API}/recognize`, {
      method: "POST",
      headers: { "Content-Type": "audio/wav" },
      body: await blob.arrayBuffer()
    });
    const result = await res.json();
    displayIdResult(result);
  } catch (e) {
    clearInterval(countdownInterval);
    micStream = null;
    let errorMsg = "Recording failed: " + e.message;
    if (e.name === "NotAllowedError" || e.name === "PermissionDeniedError") {
      errorMsg = "Recording failed: Microphone permission was denied. Please allow microphone access.";
    }
    setStatus(resultBox, errorMsg, 'error');
  } finally {
    btnRecord.classList.remove('recording');
    btnRecord.disabled = false; // 🔓 re-enable mic button
  }
};

function displayIdResult(result) {
    const resultBox = $("idResult");
    
    if (result.error) {
        setStatus(resultBox, `Error: ${result.error}`, 'error');
        return;
    }
    if (result.match === null) {
        setStatus(resultBox, "No match found. 😔", 'error');
        return;
    }
    
    let html = `<p class="match-found-label">Match Found:</p>`;
    html += `<h3 class="matched-song-title">`;
    if (result.url) {
        html += `<a href="${result.url}" target="_blank">${result.name}</a>`;
    } else {
        html += result.name;
    }
    html += ` (Score: ${result.score})</h3>`;
    


    html += `<hr class="result-separator">`;

    if (result.top && result.top.length > 1) {
    const filtered = result.top.filter(c => c.name !== result.name);
    if (filtered.length > 0) {
        html += `<p class="next-matches-label">Next best matches:</p>`;
        html += `<ul>`;
        filtered.slice(0, 3).forEach(c => {
            html += `<li>`;
            if (c.url) {
                html += `<a href="${c.url}" target="_blank">${c.name}</a>`;
            } else {
                html += c.name;
            }
            html += ` (Score: ${c.score})</li>`;
        });
        html += `</ul>`;
    }
}

    setStatus(resultBox, html, 'success');
}