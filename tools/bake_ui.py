# MIMIC — fabrique les images de l'interface native à partir de la page d'origine (ui/mimic_ui.html).
#
#   python tools/bake_ui.py [chemin de chrome.exe]
#
# La page est ouverte par Chrome sans fenêtre, figée (temps arrêté, aucun signal), et photographiée couche par couche
# au double de sa taille (1886 x 1024 pour une fenêtre de 943 x 512) :
#
#   Resources/ui/under.png    ce qui est sous le visualiseur de la gueule : le décor en 3D
#   Resources/ui/over.png     ce qui est au-dessus, sur fond transparent : gueule, plaque, fentes, sceaux, ferrures
#   Resources/ui/over_on.png  les trois fentes seules, tous leurs boutons allumés (le plugin n'en montre que les actifs)
#
# Ce qui bouge n'est pas dans les images et se dessine dans le plugin : visualiseur, aiguilles et tirets des sceaux,
# vumètres, valeurs, lueur de la gueule.
import functools, http.server, os, subprocess, sys, tempfile, threading

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'Resources', 'ui')
CHROME = sys.argv[1] if len(sys.argv) > 1 else r'C:\Program Files\Google\Chrome\Application\chrome.exe'
WIDTH, HEIGHT, SCALE = 943, 512, 2

INJECT = r'''<script>
(function () {
  var layer = new URLSearchParams(location.search).get("layer") || "all";
  // Le plugin d'origine parlait à la page par WebView2 : ici personne ne parle, la page reste au repos.
  window.chrome = window.chrome || {};
  window.chrome.webview = { postMessage: function () {}, postMessageWithAdditionalObjects: function () {}, addEventListener: function () {} };
  // Temps arrêté et hasard fixe : deux photographies de la même couche sont identiques.
  var T = 5000, raf = window.requestAnimationFrame.bind(window), seed = 12345;
  performance.now = function () { return T; };
  window.requestAnimationFrame = function (cb) { return raf(function () { cb(T); }); };
  Math.random = function () { seed = (seed * 1664525 + 1013904223) >>> 0; return seed / 4294967296; };
  var css = ".title{animation:none!important}.kn-ring,.kn-needle,.kn-v,#mawGlow2{display:none!important}";
  if (layer === "under") css += ".stage>*:not(#gl):not(#fallback){display:none!important}";
  if (layer === "over_on") css += ".stage>*:not(.slot){display:none!important}";
  if (layer === "over" || layer === "over_on") css += "html,body,.wrap,.viewport,.stage{background:transparent!important}#gl,#fallback,#cavFallback{display:none!important}";
  document.addEventListener("DOMContentLoaded", function () {
    var st = document.createElement("style"); st.textContent = css; document.head.appendChild(st);
    if (layer === "over_on") setTimeout(function () {
      document.querySelectorAll("#src button,#osc button,#freeze").forEach(function (b) { b.setAttribute("aria-pressed", "true"); });
    }, 300);
  });
})();
</script>'''


def main():
    os.makedirs(OUT, exist_ok=True)
    page = open(os.path.join(ROOT, 'ui', 'mimic_ui.html'), encoding='utf-8').read()
    head = page.index('<head>') + len('<head>')
    with tempfile.TemporaryDirectory() as work:
        with open(os.path.join(work, 'bake.html'), 'w', encoding='utf-8') as f:
            f.write(page[:head] + INJECT + page[head:])
        handler = functools.partial(http.server.SimpleHTTPRequestHandler, directory=work)
        handler.log_message = lambda *a: None
        server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        threading.Thread(target=server.serve_forever, daemon=True).start()
        for layer in ('under', 'over', 'over_on'):
            target = os.path.join(OUT, layer + '.png')
            command = [CHROME, '--headless=new', '--disable-extensions', '--hide-scrollbars', '--no-first-run',
                       '--user-data-dir=' + os.path.join(work, 'profile'),
                       f'--window-size={WIDTH},{HEIGHT}', f'--force-device-scale-factor={SCALE}',
                       '--virtual-time-budget=8000', '--screenshot=' + target]
            if layer != 'under':
                command.append('--default-background-color=00000000')
            command.append(f'http://127.0.0.1:{server.server_port}/bake.html?layer={layer}#plugin')
            subprocess.run(command, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=120)
            print(layer, os.path.getsize(target), 'octets')
        server.shutdown()


main()
