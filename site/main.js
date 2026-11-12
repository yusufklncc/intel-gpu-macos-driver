// intel-gpu-macos-driver site: language, the nav's background, scroll-in, the guide's search, and the latest release's files. MIT.
(function () {
  "use strict";
  var root = document.documentElement;
  root.classList.add("js");   // the scroll-in hides sections only when this script runs

  // ---- language: ?lang=, then the visitor's choice, then the browser's language ----
  function setLang(lang, remember) {
    root.setAttribute("data-lang", lang);
    root.setAttribute("lang", lang);
    document.querySelectorAll(".lang button").forEach(function (b) {
      b.setAttribute("aria-pressed", String(b.dataset.lang === lang));
    });
    document.querySelectorAll("[data-ph-" + lang + "]").forEach(function (el) {
      el.setAttribute("placeholder", el.getAttribute("data-ph-" + lang));
      el.setAttribute("aria-label", el.getAttribute("data-ph-" + lang));
    });
    if (remember) { try { localStorage.setItem("lang", lang); } catch (e) { /* private mode */ } }
    langHooks.forEach(function (f) { f(lang); });
  }
  var langHooks = [];
  var asked = new URLSearchParams(location.search).get("lang");
  var saved = null;
  try { saved = localStorage.getItem("lang"); } catch (e) { /* private mode */ }
  var browser = (navigator.language || "en").toLowerCase().indexOf("tr") === 0 ? "tr" : "en";
  setLang(asked === "tr" || asked === "en" ? asked : saved || browser, false);
  document.querySelectorAll(".lang button").forEach(function (b) {
    b.addEventListener("click", function () { setLang(b.dataset.lang, true); });
  });

  // ---- the small-screen menu closes after a choice ----
  document.querySelectorAll(".menu nav a").forEach(function (a) {
    a.addEventListener("click", function () { var d = a.closest("details"); if (d) d.open = false; });
  });

  // ---- the nav gets its background once the page scrolls ----
  var nav = document.querySelector(".nav");
  function onScroll() { nav.style.setProperty("--nav-fill", Math.min(1, window.scrollY / 120)); }
  window.addEventListener("scroll", onScroll, { passive: true });
  onScroll();

  // ---- scroll-in ----
  if ("IntersectionObserver" in window) {
    var io = new IntersectionObserver(function (entries) {
      entries.forEach(function (e) { if (e.isIntersecting) { e.target.classList.add("in"); io.unobserve(e.target); } });
    }, { rootMargin: "0px 0px -40px 0px" });
    document.querySelectorAll(".reveal").forEach(function (el) { io.observe(el); });
  } else {
    document.querySelectorAll(".reveal").forEach(function (el) { el.classList.add("in"); });
  }

  // ---- the guide: the page list folds away on small screens; the search runs on an index that comes with the site ----
  var docNav = document.querySelector(".doc-nav");
  if (docNav && window.matchMedia("(max-width: 860px)").matches) docNav.open = false;
  var q = document.getElementById("guide-q"), out = document.getElementById("guide-results");
  if (q && out) guideSearch(q, out);

  function guideSearch(q, out) {
    var idx = null, loading = false, sel = -1, hits = [];
    // case and accents do not count: "acilis" finds "açılış", dotted and dotless i fold to "i"; one character stays one character
    function fold(s, lang) {
      var r = "", loc = lang === "tr" ? "tr" : "en";
      for (var i = 0; i < s.length; i++) {
        var c = s.charAt(i).toLocaleLowerCase(loc).normalize("NFD").charAt(0);
        r += c === "\u0131" ? "i" : c;
      }
      return r;
    }
    function lang() { return root.getAttribute("data-lang") === "tr" ? 1 : 0; }
    function load(then) {
      if (idx) return then();
      if (loading) return;
      loading = true;
      var s = document.createElement("script");
      s.src = "search-index.js";
      s.onload = function () {
        idx = [];
        (window.GUIDE_INDEX && window.GUIDE_INDEX.pages || []).forEach(function (p) {
          p.s.forEach(function (sec) {
            idx.push({ f: p.f, id: sec.id, page: p.t, h: sec.h, t: sec.t,
              fp: [fold(p.t[0], "en"), fold(p.t[1], "tr")], fh: [fold(sec.h[0], "en"), fold(sec.h[1], "tr")],
              ft: [fold(sec.t[0], "en"), fold(sec.t[1], "tr")] });
          });
        });
        then();
      };
      s.onerror = function () { loading = false; };
      document.head.appendChild(s);
    }
    function esc(s) { return s.replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); }
    function snippet(text, ftext, words) {
      var at = -1, w = "";
      words.forEach(function (x) { var i = ftext.indexOf(x); if (i !== -1 && (at === -1 || i < at)) { at = i; w = x; } });
      if (at === -1) return esc(text.slice(0, 140)) + (text.length > 140 ? "…" : "");
      var from = Math.max(0, at - 50), to = Math.min(text.length, at + w.length + 90);
      var part = text.slice(from, to), fpart = ftext.slice(from, to), html = "", i = 0;
      while (i < part.length) {
        var m = null;
        words.forEach(function (x) { if (!m && fpart.substr(i, x.length) === x) m = x; });
        if (m) { html += "<mark>" + esc(part.substr(i, m.length)) + "</mark>"; i += m.length; }
        else { html += esc(part.charAt(i)); i++; }
      }
      return (from > 0 ? "…" : "") + html + (to < text.length ? "…" : "");
    }
    function run() {
      var L = lang(), words = fold(q.value.trim(), L ? "tr" : "en").split(/\s+/).filter(function (w) { return w.length > 1; });
      sel = -1;
      if (!words.length) { out.hidden = true; out.innerHTML = ""; return; }
      hits = [];
      idx.forEach(function (e) {
        var score = 0;
        for (var k = 0; k < words.length; k++) {
          var w = words[k], inH = e.fh[L].indexOf(w) !== -1, inP = e.fp[L].indexOf(w) !== -1, inT = e.ft[L].indexOf(w) !== -1;
          if (!inH && !inP && !inT) return;
          score += (inH ? 6 : 0) + (inP ? 3 : 0) + (inT ? 1 : 0);
        }
        hits.push({ e: e, score: score });
      });
      hits.sort(function (a, b) { return b.score - a.score; });
      hits = hits.slice(0, 8);
      if (!hits.length) {
        out.innerHTML = "<p>" + (L ? "Bu rehberde bulunamadı." : "Nothing in the guide.") + "</p>";
      } else {
        out.innerHTML = hits.map(function (h) {
          var e = h.e, title = e.page[L] + (e.h[L] ? " › " + e.h[L] : "");
          return '<a href="' + e.f + (e.id ? "#" + e.id : "") + '"><b>' + esc(title) + "</b>" + snippet(e.t[L], e.ft[L], words) + "</a>";
        }).join("");
      }
      out.hidden = false;
    }
    function mark() {
      out.querySelectorAll("a").forEach(function (a, i) { a.classList.toggle("sel", i === sel); });
    }
    q.addEventListener("focus", function () { load(function () {}); });
    q.addEventListener("input", function () { load(run); });
    q.addEventListener("keydown", function (ev) {
      var links = out.querySelectorAll("a");
      if (ev.key === "ArrowDown" && links.length) { sel = Math.min(links.length - 1, sel + 1); mark(); ev.preventDefault(); }
      else if (ev.key === "ArrowUp" && links.length) { sel = Math.max(0, sel - 1); mark(); ev.preventDefault(); }
      else if (ev.key === "Enter" && links.length) { location.href = links[Math.max(0, sel)].getAttribute("href"); ev.preventDefault(); }
      else if (ev.key === "Escape") { q.value = ""; run(); }
    });
    out.addEventListener("click", function () { out.hidden = true; });
    langHooks.push(function () { if (idx && q.value) run(); });
  }

  // ---- the latest release: version, file links and sizes (the page keeps its built-in ones if GitHub does not answer) ----
  // only pages that show them ask GitHub (the home page's download section)
  if (!document.querySelector("[data-version], [data-asset]")) return;
  function mb(bytes) { return bytes >= 1e6 ? (bytes / 1e6).toFixed(bytes >= 1e8 ? 0 : 1) + " MB" : Math.max(1, Math.round(bytes / 1e3)) + " KB"; }
  fetch("https://api.github.com/repos/yusufklncc/intel-gpu-macos-driver/releases/latest", { headers: { Accept: "application/vnd.github+json" } })
    .then(function (r) { return r.ok ? r.json() : Promise.reject(r.status); })
    .then(function (rel) {
      var version = String(rel.tag_name || "").replace(/^v/, "");
      if (!version) return;
      document.querySelectorAll("[data-version]").forEach(function (el) { el.textContent = version; });
      document.querySelectorAll("[data-asset]").forEach(function (el) {
        var suffix = el.getAttribute("data-asset");   // a part of the file name
        var asset = (rel.assets || []).filter(function (a) { return a.name.indexOf(suffix) !== -1; })[0];
        if (!asset) return;
        el.setAttribute("href", asset.browser_download_url);
        var name = el.querySelector(".name"); if (name) name.textContent = asset.name;
        var size = el.querySelector(".size"); if (size) size.textContent = mb(asset.size);
      });
    })
    .catch(function () { /* offline or rate-limited: the links to the release page stay */ });
})();
