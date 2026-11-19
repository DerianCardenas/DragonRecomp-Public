// DragonRecomp Studio — Tauri v2 Interactive UI Prototype (Mock Data)

const CONSOLE_COLORS = {
  nes:  { bg: '#EF4444', ink: '#FFFFFF', label: 'NES · 6502' },
  snes: { bg: '#A855F7', ink: '#FFFFFF', label: 'SNES · 65816' },
  gb:   { bg: '#22C55E', ink: '#071014', label: 'GB/GBC · SM83' },
  gba:  { bg: '#3B82F6', ink: '#FFFFFF', label: 'GBA · ARM7TDMI' },
};

const GAMES = [
  { id: 1, console: 'nes',  code: 'SMB3', title: 'Super Mario Bros. 3',       time: '4h 12m', funcs: '1,482', size: '4.2 MB', mode: 'NATIVO C99' },
  { id: 2, console: 'nes',  code: 'CTRA', title: 'Contra',                    time: '1h 45m', funcs: '624',   size: '3.1 MB', mode: 'NATIVO C99' },
  { id: 3, console: 'nes',  code: 'ZLD1', title: 'The Legend of Zelda',       time: '6h 05m', funcs: '910',   size: '3.4 MB', mode: 'NATIVO C99' },
  { id: 4, console: 'snes', code: 'SMW',  title: 'Super Mario World',         time: '8h 30m', funcs: '2,810', size: '5.8 MB', mode: 'NATIVO C99' },
  { id: 5, console: 'snes', code: 'SMTR', title: 'Super Metroid',             time: '3h 20m', funcs: '3,412', size: '6.9 MB', mode: 'NATIVO C99' },
  { id: 6, console: 'gb',   code: 'TTRS', title: 'Tetris (DX)',               time: '2h 10m', funcs: '412',   size: '2.8 MB', mode: 'NATIVO C99' },
  { id: 7, console: 'gb',   code: 'PKCR', title: 'Pokemon Crystal',           time: '12h 40m',funcs: '4,120', size: '6.1 MB', mode: 'NATIVO C99' },
  { id: 8, console: 'gba',  code: 'PKFR', title: 'Pokemon FireRed',           time: '9h 15m', funcs: '6,840', size: '9.4 MB', mode: 'NATIVO C99' },
  { id: 9, console: 'gba',  code: 'MTRZ', title: 'Metroid Zero Mission',      time: '3h 55m', funcs: '5,290', size: '8.7 MB', mode: 'NATIVO C99' },
];

let activeConsole = 'nes';
let selectedGame = GAMES[0];
let compileCores = 2;
let compileOpt = '-O1';

const gridEl = document.getElementById('gameGrid');
const searchEl = document.getElementById('searchInput');
const toastEl = document.getElementById('toast');

function showToast(msg) {
  toastEl.textContent = msg;
  toastEl.classList.add('show');
  clearTimeout(showToast._t);
  showToast._t = setTimeout(() => toastEl.classList.remove('show'), 2400);
}

function renderInspector(g) {
  const c = CONSOLE_COLORS[g.console];
  const tag = document.getElementById('inspTag');
  tag.textContent = c.label;
  tag.style.background = c.bg;
  tag.style.color = c.ink;

  document.getElementById('inspTitle').textContent = g.title;
  document.getElementById('inspTime').textContent = g.time;
  document.getElementById('inspMode').textContent = g.mode;
  document.getElementById('inspFuncs').textContent = g.funcs;
  document.getElementById('inspSize').textContent = g.size;
}

function renderGrid() {
  const q = searchEl.value.trim().toLowerCase();
  const filtered = GAMES.filter(g => g.console === activeConsole && g.title.toLowerCase().includes(q));
  const c = CONSOLE_COLORS[activeConsole];

  gridEl.innerHTML = '';
  for (const g of filtered) {
    const card = document.createElement('div');
    card.className = 'game-card' + (selectedGame && selectedGame.id === g.id ? ' selected' : '');
    card.innerHTML = `
      <div class="cover-box">
        <div class="cover-stripe" style="background:${c.bg}"></div>
        <div class="cover-art-icon">${g.code}</div>
        <div class="cover-meta-line">${g.time} · ${g.size}</div>
      </div>
      <div class="game-name">${g.title}</div>
    `;
    card.addEventListener('click', () => {
      selectedGame = g;
      renderGrid();
      renderInspector(g);
    });
    gridEl.appendChild(card);
  }

  const addCard = document.createElement('div');
  addCard.className = 'game-card add-card';
  addCard.innerHTML = `
    <div class="cover-box">
      <div class="plus">+</div>
      <div class="cover-meta-line" style="color:${c.bg}">AÑADIR JUEGO</div>
    </div>
    <div class="game-name" style="color:var(--text-muted)">Importar portable</div>
  `;
  addCard.addEventListener('click', () => showToast('Prototipo: abriría diálogo de carpeta portable'));
  gridEl.appendChild(addCard);
}

// Top navigation
document.querySelectorAll('.nav-tab').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.nav-tab').forEach(b => b.classList.remove('active'));
    document.querySelectorAll('.view').forEach(v => v.classList.remove('active'));
    btn.classList.add('active');
    document.getElementById('view-' + btn.dataset.view).classList.add('active');
  });
});

// Console tabs
document.querySelectorAll('.console-tab').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('.console-tab').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    activeConsole = btn.dataset.console;
    const first = GAMES.find(g => g.console === activeConsole);
    if (first) {
      selectedGame = first;
      renderInspector(first);
    }
    renderGrid();
  });
});

searchEl.addEventListener('input', renderGrid);

// Settings interactions
function updateCoreUI() {
  document.getElementById('coreVal').textContent = String(compileCores);
  document.getElementById('recompCoreBadge').textContent = `${compileCores} NÚCLEOS · ${compileOpt}`;
}
document.getElementById('coreDec').addEventListener('click', () => {
  compileCores = Math.max(1, compileCores - 1);
  updateCoreUI();
});
document.getElementById('coreInc').addEventListener('click', () => {
  compileCores = Math.min(16, compileCores + 1);
  updateCoreUI();
});
document.querySelectorAll('#optGroup .seg-btn').forEach(btn => {
  btn.addEventListener('click', () => {
    document.querySelectorAll('#optGroup .seg-btn').forEach(b => b.classList.remove('active'));
    btn.classList.add('active');
    compileOpt = btn.dataset.opt;
    updateCoreUI();
  });
});

// Mock actions
document.getElementById('btnPlay').addEventListener('click', () => showToast(`▶ Ejecutando binario nativo: ${selectedGame.title}`));
document.getElementById('btnImport').addEventListener('click', () => showToast('Prototipo: Selector de carpeta de juego recompilado'));
document.getElementById('btnCovers').addEventListener('click', () => showToast('Prototipo: Buscando carátulas en libretro-thumbnails...'));
document.getElementById('btnOpenFolder').addEventListener('click', () => showToast('Prototipo: Abriendo carpeta portable'));
document.getElementById('btnOpenRecomp').addEventListener('click', () => {
  document.querySelector('.nav-tab[data-view="recompiler"]').click();
});
document.getElementById('btnSelectRom').addEventListener('click', () => showToast('Prototipo: Selector de ROM (.nes/.sfc/.gb/.gba)'));

renderInspector(selectedGame);
renderGrid();
