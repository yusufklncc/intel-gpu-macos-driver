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

  // ---- apps.html: the screenshot galleries (dark by default, the light set behind a switch) and the lightbox ----
  if (document.querySelector(".gal")) galleries();

  function galleries() {
    var KEY = "shotTheme";
    var gals = Array.prototype.slice.call(document.querySelectorAll(".gal"));
    // a gallery gets the switch only when every picture has a light one
    var themed = gals.filter(function (g) {
      var items = g.querySelectorAll(".gal-item");
      return items.length && Array.prototype.every.call(items, function (a) { return a.hasAttribute("data-light"); });
    });
    function setTheme(theme, remember) {
      themed.forEach(function (g) {
        g.querySelectorAll(".gal-item").forEach(function (a) {
          var pic = a.querySelector("img");
          if (!a.hasAttribute("data-dark")) a.setAttribute("data-dark", a.getAttribute("href"));
          var src = a.getAttribute(theme === "light" ? "data-light" : "data-dark");
          a.setAttribute("href", src);
          if (pic) pic.setAttribute("src", src);
        });
        g.querySelectorAll("[data-shot-theme]").forEach(function (b) {
          b.setAttribute("aria-pressed", String(b.getAttribute("data-shot-theme") === theme));
        });
      });
      if (remember) { try { localStorage.setItem(KEY, theme); } catch (e) { /* private mode */ } }
    }
    themed.forEach(function (g) {
      var sw = g.querySelector(".shot-theme");
      if (sw) sw.hidden = false;
      g.querySelectorAll("[data-shot-theme]").forEach(function (b) {
        b.addEventListener("click", function () { setTheme(b.getAttribute("data-shot-theme"), true); });
      });
    });
    var saved = null;
    try { saved = localStorage.getItem(KEY); } catch (e) { /* private mode */ }
    if (saved === "light") setTheme("light", false);

    // the lightbox: a modal <dialog>, so focus stays inside and Esc closes it; without one the links open the picture
    var dlg = document.createElement("dialog");
    if (typeof dlg.showModal !== "function") return;
    var LABELS = { en: { box: "Screenshot", prev: "Previous", next: "Next", close: "Close" },
                   tr: { box: "Ekran görüntüsü", prev: "Önceki", next: "Sonraki", close: "Kapat" } };
    function icon(d) { return '<svg viewBox="0 0 24 24" aria-hidden="true"><path d="' + d + '"/></svg>'; }
    dlg.className = "lb";
    dlg.innerHTML = '<button type="button" class="lb-close">' + icon("M6 6l12 12M18 6L6 18") + "</button>" +
      '<div class="lb-stage"><img class="lb-img" alt=""></div>' +
      '<div class="lb-bar"><button type="button" class="lb-prev">' + icon("M15 5l-7 7 7 7") + "</button>" +
      '<p class="lb-cap" aria-live="polite"></p><span class="lb-count"></span>' +
      '<button type="button" class="lb-next">' + icon("M9 5l7 7-7 7") + "</button></div>";
    document.body.appendChild(dlg);
    var img = dlg.querySelector(".lb-img"), cap = dlg.querySelector(".lb-cap"), count = dlg.querySelector(".lb-count");
    var bPrev = dlg.querySelector(".lb-prev"), bNext = dlg.querySelector(".lb-next"), bClose = dlg.querySelector(".lb-close");
    var list = [], at = 0, opener = null;
    function labels() {
      var L = LABELS[root.getAttribute("data-lang") === "tr" ? "tr" : "en"];
      dlg.setAttribute("aria-label", L.box);
      bPrev.setAttribute("aria-label", L.prev); bNext.setAttribute("aria-label", L.next); bClose.setAttribute("aria-label", L.close);
    }
    function show(i) {
      at = (i + list.length) % list.length;
      var a = list[at], pic = a.querySelector("img"), fig = a.closest("figure");
      var fc = fig && fig.querySelector("figcaption");
      img.src = a.getAttribute("href");
      img.alt = pic ? pic.alt : "";
      cap.innerHTML = fc ? fc.innerHTML : "";
      count.textContent = list.length > 1 ? (at + 1) + " / " + list.length : "";
      bPrev.hidden = bNext.hidden = list.length < 2;
      if (list.length > 1) { var n = new Image(); n.src = list[(at + 1) % list.length].getAttribute("href"); }
    }
    function open(a) {
      list = Array.prototype.slice.call(a.closest(".gal").querySelectorAll(".gal-item"));
      opener = a;
      labels();
      show(list.indexOf(a));
      root.classList.add("lb-open");
      dlg.showModal();
      bClose.focus();
    }
    gals.forEach(function (g) {
      g.addEventListener("click", function (ev) {
        var a = ev.target.closest(".gal-item");
        if (!a || ev.button !== 0 || ev.metaKey || ev.ctrlKey || ev.shiftKey || ev.altKey) return;
        ev.preventDefault();
        open(a);
      });
    });
    bPrev.addEventListener("click", function () { show(at - 1); });
    bNext.addEventListener("click", function () { show(at + 1); });
    bClose.addEventListener("click", function () { dlg.close(); });
    dlg.addEventListener("keydown", function (ev) {
      if (ev.key === "ArrowLeft") { show(at - 1); ev.preventDefault(); }
      else if (ev.key === "ArrowRight") { show(at + 1); ev.preventDefault(); }
    });
    // a click beside the picture (not on it, the caption or a button) closes it
    dlg.addEventListener("click", function (ev) {
      var t = ev.target;
      if (t === dlg || t.classList.contains("lb-stage") || t.classList.contains("lb-bar")) dlg.close();
    });
    dlg.addEventListener("close", function () {
      root.classList.remove("lb-open");
      if (opener) opener.focus();
    });
    // phones: a swipe sideways shows the next or the previous picture
    var x0 = null, y0 = 0;
    dlg.addEventListener("touchstart", function (ev) {
      if (ev.touches.length === 1) { x0 = ev.touches[0].clientX; y0 = ev.touches[0].clientY; } else { x0 = null; }
    }, { passive: true });
    dlg.addEventListener("touchend", function (ev) {
      if (x0 === null || !ev.changedTouches.length) return;
      var dx = ev.changedTouches[0].clientX - x0, dy = ev.changedTouches[0].clientY - y0;
      x0 = null;
      if (list.length > 1 && Math.abs(dx) > 50 && Math.abs(dx) > 1.5 * Math.abs(dy)) show(dx < 0 ? at + 1 : at - 1);
    }, { passive: true });
  }

  function guideSearch(q, out) {
    var idx = null, loading = false, pending = null, sel = -1, hits = [];
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
      pending = then;              // the latest request runs once the index is there
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
        if (pending) pending();
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
