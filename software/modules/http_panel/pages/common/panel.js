// Shared by every panel page: header, tab bar, status chip and HTML escaping.
// Pages load this right after <header id="panel-header"></header> so the
// header is built before the page's own scripts run.

// Order here is the tab order; adding a tab means adding its row.
var PANEL_TABS = [
  ['/', 'Dashboard', 'M3 3h7v9H3zM14 3h7v5h-7zM14 12h7v9h-7zM3 16h7v5H3z'],
  ['/network', 'Network', 'M5 12.55a11 11 0 0 1 14 0M1.42 9a16 16 0 0 1 21.16 0M8.53 16.11a6 6 0 0 1 6.95 0M12 20h.01'],
  ['/camera', 'Camera', 'M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2zM12 9a4 4 0 1 0 0 8 4 4 0 0 0 0-8z'],
  ['/io', 'IO', 'M7 7h10v10H7zM9 2v5M15 2v5M9 17v5M15 17v5M2 9h5M2 15h5M17 9h5M17 15h5'],
  ['/console', 'Console', 'M4 17l6-6-6-6M12 19h8'],
  ['/ota', 'OTA', 'M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4M17 8l-5-5-5 5M12 3v12'],
  ['/data', 'Data', 'M3 5c0-1.66 4-3 9-3s9 1.34 9 3-4 3-9 3-9-1.34-9-3zM21 12c0 1.66-4 3-9 3s-9-1.34-9-3M3 5v14c0 1.66 4 3 9 3s9-1.34 9-3V5'],
  ['/files', 'Files', 'M22 19a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2h5l2 3h9a2 2 0 0 1 2 2z'],
  ['/sysinfo', 'System', 'M6 4h12a2 2 0 0 1 2 2v12a2 2 0 0 1-2 2H6a2 2 0 0 1-2-2V6a2 2 0 0 1 2-2zM9 9h6v6H9zM9 1v3M15 1v3M9 20v3M15 20v3M20 9h3M20 14h3M1 9h3M1 14h3']
];

var PANEL_LOGO = 'M23 19a2 2 0 0 1-2 2H3a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h4l2-3h6l2 3h4a2 2 0 0 1 2 2zM12 9a4 4 0 1 0 0 8 4 4 0 0 0 0-8z';

function esc(s) {
  return String(s).replace(/[&<>"]/g, function (c) {
    return { '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' }[c];
  });
}

function panelIcon(path) {
  return '<svg viewBox="0 0 24 24" fill="none" stroke="currentColor" stroke-width="2"'
    + ' stroke-linecap="round" stroke-linejoin="round"><path d="' + path + '"/></svg>';
}

// state: 'ok' (connected), 'busy' (connecting / working), 'error'.
function panelStatus(text, state) {
  var chip = document.getElementById('panel-status');

  if (!chip) { return; }

  chip.className = 'chip ' + (state || 'busy');
  chip.firstChild.textContent = text;
  chip.title = text;
}

(function () {
  var header = document.getElementById('panel-header');

  if (!header) { return; }

  var tabs = '';

  PANEL_TABS.forEach(function (tab) {
    var active = (tab[0] === location.pathname) ? ' class="active"' : '';
    tabs += '<a href="' + tab[0] + '"' + active + '>' + panelIcon(tab[2]) + esc(tab[1]) + '</a>';
  });

  header.className = 'panel-header';
  header.innerHTML =
    '<div class="panel-bar">'
    + '<div class="panel-top">'
    + '<div class="brand"><div class="brand-logo">' + panelIcon(PANEL_LOGO) + '</div>'
    + '<div>ESP32S3<small id="panel-device">' + esc(location.host) + '</small></div></div>'
    + '<span class="chip busy" id="panel-status"><span>Connecting...</span></span>'
    + '</div>'
    + '<nav class="tabs">' + tabs + '</nav>'
    + '</div>';

  var active = header.querySelector('.tabs a.active');

  if (active) { active.scrollIntoView({ block: 'nearest', inline: 'center' }); }

  // Firmware version for the subtitle; the header still works without it.
  fetch('/ota/info').then(function (r) { return r.json(); }).then(function (d) {
    (d.info || []).forEach(function (item) {
      if (item.label === 'Firmware') {
        document.getElementById('panel-device').textContent =
          location.host + ' · ' + item.value;
      }
    });
  }).catch(function () { });
})();
