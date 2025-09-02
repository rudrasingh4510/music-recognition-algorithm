const API = "http://104.214.180.203:5001";

function $(id){ return document.getElementById(id); }

function setStatus(element, message, type) {
  const isCard = element.id === 'idResult' || element.id === 'ytStatus';
  let contentContainer = element;
  if (element.id === 'idResult') contentContainer = element.querySelector('.result-content');
  else if (element.id === 'ytStatus') contentContainer = element.querySelector('.status-content');
  if (!contentContainer) return;

  contentContainer.innerHTML = '';
  element.className = 'status-bar';
  if (element.id === 'idResult') element.classList.add('result-card');

  if (isCard) {
    const closeBtn = element.querySelector('.close-btn');
    if (closeBtn) closeBtn.style.display = 'none';
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

// Dropdown
const dbToggleBtn = $("dbToggleBtn");
const dbDropdownContent = $("dbDropdownContent");
dbToggleBtn.onclick = () => dbDropdownContent.classList.toggle("show");
window.onclick = (event) => {
  if (!dbToggleBtn.contains(event.target)) {
    dbDropdownContent.classList.remove('show');
  }
};

// Add YouTube song
$("btnAddYtSong").onclick = async () => {
  const url = $("ytUrl").value.trim();
  if (!url) return alert("Please enter a YouTube URL");
  const name = $("ytName").value.trim(); 
  setStatus($("ytStatus"), "Requesting download...", 'loading');
  $("btnAddYtSong").disabled = true;

  try {
    const res = await fetch(`${API}/add-youtube`, {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ url, name })
    });
    
    if(res.ok) {
        const data = await res.json();
        setStatus($("ytStatus"), `Added: ${data.name}`, 'success');
        refreshSongs();
        $("ytUrl").value = "";
        $("ytName").value = "";
    } else {
        const errorData = await res.json();
        setStatus($("ytStatus"), `Error: ${errorData.error || 'Unknown error'}`, 'error');
    }
  } catch (e) {
    setStatus($("ytStatus"), "Error: Failed to connect to server.", 'error');
  } finally {
    $("btnAddYtSong").disabled = false;
  }
};

// Refresh DB list
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
  }
}
refreshSongs();

// --- Mic + Recording Logic ---
let micStream = null; // This will now persist after the first successful recording.

const btnRecord = $("btnRecord");
btnRecord.onclick = async () => {
  const secs = 5;
  const resultBox = $("idResult");
  let countdownInterval;

  try {
    btnRecord.classList.add('recording');
    setStatus(resultBox, "", null);

    // Get microphone stream only if it doesn't exist yet.
    if (!micStream) {
      setStatus(resultBox, "Requesting microphone access...", 'loading');
      micStream = await navigator.mediaDevices.getUserMedia({ audio: true });
    }

    // Ensure all tracks are enabled before recording.
    micStream.getTracks().forEach(track => track.enabled = true);

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

    // IMPORTANT: Disable tracks immediately after recording to "turn off" the mic indicator.
    micStream.getTracks().forEach(track => track.enabled = false);

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
    // On error, disable tracks but do not nullify the stream.
    if (micStream) {
        micStream.getTracks().forEach(track => track.enabled = false);
    }
    // If permission was denied, we must reset and inform the user.
    if (e.name === 'NotAllowedError' || e.name === 'PermissionDeniedError') {
        micStream = null;
        setStatus(resultBox, 'Microphone access was denied. Please allow it in your browser settings.', 'error');
    } else {
        setStatus(resultBox, `Recording failed: ${e.message}`, 'error');
    }
  } finally {
    btnRecord.classList.remove('recording');
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

  let html = `<h3>Match Found: `;
  if (result.url) {
    html += `<a href="${result.url}" target="_blank">${result.name}</a> [score - ${result.score}]`;
  } else {
    html += `${result.name} [score - ${result.score}]`;
  }
  html += `</h3>`;

  if (result.top && result.top.length > 1) {
    html += `<hr class="result-separator">`;
    html += `<p class="next-matches-label">Next best matches:</p>`;
    html += `<ul>`;
    result.top.slice(1, 3).forEach(c => {
      html += `<li>`;
      if (c.url) {
        html += `<a href="${c.url}" target="_blank">${c.name}</a> [score - ${c.score}]`;
      } else {
        html += `${c.name} [score - ${c.score}]`;
      }
      html += `</li>`;
    });
    html += `</ul>`;
  }

  setStatus(resultBox, html, 'success');
}