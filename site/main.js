// intel-gpu-macos-driver site: language, the nav's background, scroll-in, the guide's search and components (tabs, code
// copy, "On this page"), the screenshots' lightbox, and the latest release's files. MIT.
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
    // labels for screen readers in both languages: data-al-en / data-al-tr
    document.querySelectorAll("[data-al-" + lang + "]").forEach(function (el) {
      el.setAttribute("aria-label", el.getAttribute("data-al-" + lang));
    });
    document.querySelectorAll("img[data-alt-" + lang + "]").forEach(function (el) {
      el.setAttribute("alt", el.getAttribute("data-alt-" + lang));
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

  // ---- the guide's pages: tabs, code blocks, "On this page", links into hidden tabs and folds ----
  var article = document.querySelector("main.doc .doc-body");
  if (article && document.getElementById("guide-q")) guidePage(article);

  // ---- apps.html and the guide: the screenshot galleries (dark by default, the light set behind a switch) and the lightbox ----
  if (document.querySelector(".gal")) galleries();

  function curLang() { return root.getAttribute("data-lang") === "tr" ? "tr" : "en"; }
  function escHtml(s) { return String(s).replace(/[&<>"]/g, function (c) { return { "&": "&amp;", "<": "&lt;", ">": "&gt;", '"': "&quot;" }[c]; }); }
  function both(en, tr) { return '<span lang="en">' + escHtml(en) + '</span><span lang="tr">' + escHtml(tr) + "</span>"; }
  function label(el, en, tr) {
    el.setAttribute("data-al-en", en); el.setAttribute("data-al-tr", tr);
    el.setAttribute("aria-label", curLang() === "tr" ? tr : en);
  }
  function icon(name) { return '<span class="ico" style="--i: var(--i-' + name + ')" aria-hidden="true"></span>'; }
  function shown(el) { return !!(el && el.getClientRects().length); }

  function guidePage(article) {
    article.setAttribute("data-gal-scope", "");
    var tabSets = tabs();
    codeBlocks(article);
    var spy = toc(article);

    // a link to an id inside a hidden tab panel opens that panel; inside a closed fold, opens the fold
    function reveal(el) {
      for (var n = el; n && n !== document.body; n = n.parentElement) {
        if (n.classList && n.classList.contains("tab-panel") && n.hidden) {
          tabSets.forEach(function (t) { var i = t.panels.indexOf(n); if (i !== -1) t.select(i, false); });
        }
        if (n.tagName === "DETAILS" && !n.open) n.open = true;
      }
      if (spy) spy();
    }
    function target(hash) {
      if (!hash || hash.length < 2) return null;
      try { return document.getElementById(decodeURIComponent(hash.slice(1))); } catch (e) { return null; }
    }
    function goTo(hash) {
      var t = target(hash);
      if (!t) return;
      var hidden = !shown(t);
      reveal(t);
      if (hidden) t.scrollIntoView();
    }
    document.addEventListener("click", function (ev) {
      var a = ev.target.closest && ev.target.closest("a[href*='#']");
      if (!a || a.origin !== location.origin || a.pathname !== location.pathname) return;
      var t = target(a.hash);
      if (t) reveal(t);   // before the browser scrolls to it
    });
    window.addEventListener("hashchange", function () { goTo(location.hash); });
    if (location.hash) {
      goTo(location.hash);
      // the browser's own jump ran before the panel was open
      window.addEventListener("load", function () { var t = target(location.hash); if (t) t.scrollIntoView(); });
    }
  }

  // tab sets: <div class="tabs" data-tabs="group"> with children .tab-panel (data-tab, data-tab-icon, data-tab-en, data-tab-tr)
  function tabs() {
    var sets = [];
    document.querySelectorAll(".tabs").forEach(function (box, n) {
      var panels = Array.prototype.filter.call(box.children, function (c) { return c.classList.contains("tab-panel"); });
      if (panels.length < 2) return;
      var group = box.getAttribute("data-tabs") || "tabs" + n, key = "tabs:" + group;
      var base = box.id || "tabs-" + n;
      var list = document.createElement("div");
      list.className = "tab-list";
      list.setAttribute("role", "tablist");
      label(list, box.getAttribute("data-al-en") || "Ways to do it", box.getAttribute("data-al-tr") || "Yapmanın yolları");
      var keys = [];
      var btns = panels.map(function (p, i) {
        var k = p.getAttribute("data-tab") || String(i);
        keys.push(k);
        if (!p.id) p.id = base + "-" + k;
        var b = document.createElement("button");
        b.type = "button";
        b.id = base + "-tab-" + k;
        b.setAttribute("role", "tab");
        b.setAttribute("aria-controls", p.id);
        b.innerHTML = (p.getAttribute("data-tab-icon") ? icon(p.getAttribute("data-tab-icon")) : "") +
          both(p.getAttribute("data-tab-en") || k, p.getAttribute("data-tab-tr") || p.getAttribute("data-tab-en") || k);
        p.setAttribute("role", "tabpanel");
        p.setAttribute("aria-labelledby", b.id);
        b.addEventListener("click", function () { select(i, true); });
        list.appendChild(b);
        return b;
      });
      function select(i, remember) {
        btns.forEach(function (b, j) {
          b.setAttribute("aria-selected", String(i === j));
          b.tabIndex = i === j ? 0 : -1;
          panels[j].hidden = i !== j;
        });
        if (remember) { try { localStorage.setItem(key, keys[i]); } catch (e) { /* private mode */ } }
        window.dispatchEvent(new Event("tabchange"));
      }
      list.addEventListener("keydown", function (ev) {
        var i = btns.indexOf(document.activeElement), j = -1;
        if (i === -1) return;
        if (ev.key === "ArrowRight" || ev.key === "ArrowDown") j = (i + 1) % btns.length;
        else if (ev.key === "ArrowLeft" || ev.key === "ArrowUp") j = (i - 1 + btns.length) % btns.length;
        else if (ev.key === "Home") j = 0;
        else if (ev.key === "End") j = btns.length - 1;
        if (j === -1) return;
        ev.preventDefault();
        select(j, true);
        btns[j].focus();
      });
      box.insertBefore(list, panels[0]);
      box.classList.add("tabs-on");
      var saved = null;
      try { saved = localStorage.getItem(key); } catch (e) { /* private mode */ }
      select(Math.max(0, keys.indexOf(saved)), false);
      sets.push({ panels: panels, select: select });
    });
    return sets;
  }

  // code blocks: a Copy button (the command lines only) and one span per line, for a hanging indent when a line wraps
  function codeBlocks(scope) {
    var live = document.createElement("p");
    live.className = "sr-only";
    live.setAttribute("aria-live", "polite");
    document.body.appendChild(live);
    scope.querySelectorAll("pre").forEach(function (pre) {
      if (pre.classList.contains("out") || pre.closest(".expect") || pre.parentElement.classList.contains("code")) return;
      var code = pre.querySelector("code") || pre;
      var text = commands(code);
      lines(code);
      if (!text) return;
      var box = document.createElement("div");
      box.className = "code";
      if (pre.hasAttribute("lang")) box.setAttribute("lang", pre.getAttribute("lang"));
      pre.parentNode.insertBefore(box, pre);
      box.appendChild(pre);
      var b = document.createElement("button");
      b.type = "button";
      b.className = "copy";
      b.innerHTML = icon("copy") + '<span class="copy-t">' + both("Copy", "Kopyala") + "</span>";
      label(b, "Copy the commands", "Komutları kopyala");
      var timer = 0;
      b.addEventListener("click", function () {
        copy(text, function (ok) {
          var tr = curLang() === "tr";
          b.querySelector(".copy-t").innerHTML = ok ? both("Copied", "Kopyalandı") : both("Select and copy", "Seçip kopyalayın");
          b.classList.toggle("done", ok);
          live.textContent = ok ? (tr ? "Komutlar kopyalandı" : "Commands copied") : "";
          clearTimeout(timer);
          timer = setTimeout(function () {
            b.querySelector(".copy-t").innerHTML = both("Copy", "Kopyala");
            b.classList.remove("done");
            live.textContent = "";
          }, 1800);
        });
      });
      box.appendChild(b);
    });
  }
  // the text to copy: without the comments (span.c) and without comment-only or empty lines
  function commands(code) {
    var c = code.cloneNode(true);
    c.querySelectorAll(".c").forEach(function (x) { x.parentNode.removeChild(x); });
    return c.textContent.split("\n").map(function (l) { return l.replace(/\s+$/, ""); })
      .filter(function (l) { return l && !/^\s*#/.test(l); }).join("\n");
  }
  function lines(code) {
    var nodes = Array.prototype.slice.call(code.childNodes);
    if (nodes.some(function (n) { return n.nodeType === 1 && n.textContent.indexOf("\n") !== -1; })) return;
    var out = [], cur = document.createElement("span");
    function push() { cur.className = "ln"; out.push(cur); cur = document.createElement("span"); }
    nodes.forEach(function (n) {
      if (n.nodeType === 3) {
        n.nodeValue.split("\n").forEach(function (part, i) {
          if (i) push();
          if (part) cur.appendChild(document.createTextNode(part));
        });
      } else {
        cur.appendChild(n);
      }
    });
    push();
    while (out.length && !out[out.length - 1].textContent) out.pop();
    code.textContent = "";
    out.forEach(function (s) { if (!s.firstChild) s.textContent = " "; code.appendChild(s); });
  }
  function copy(text, done) {
    function fallback() {
      var t = document.createElement("textarea");
      t.value = text;
      t.setAttribute("readonly", "");
      t.style.position = "fixed"; t.style.opacity = "0";
      document.body.appendChild(t);
      t.select();
      var ok = false;
      try { ok = document.execCommand("copy"); } catch (e) { ok = false; }
      document.body.removeChild(t);
      done(ok);
    }
    if (navigator.clipboard && window.isSecureContext) {
      navigator.clipboard.writeText(text).then(function () { done(true); }, fallback);
    } else {
      fallback();
    }
  }

  // "On this page": the article's sections, the one being read marked (wide screens; CSS hides it below 1240 px)
  function toc(article) {
    var main = article.closest(".doc");
    var heads = Array.prototype.slice.call(article.querySelectorAll("section[id] > h2"));
    if (!main || heads.length < 2) return null;
    var nav = document.createElement("nav");
    nav.className = "doc-toc";
    label(nav, "On this page", "Bu sayfada");
    nav.innerHTML = '<p class="toc-h">' + both("On this page", "Bu sayfada") + "</p><ol></ol>";
    var ol = nav.querySelector("ol");
    var links = heads.map(function (h) {
      var li = document.createElement("li"), a = document.createElement("a");
      a.href = "#" + h.parentNode.id;
      a.innerHTML = h.innerHTML;
      a.querySelectorAll("[id]").forEach(function (x) { x.removeAttribute("id"); });
      li.appendChild(a);
      ol.appendChild(li);
      return a;
    });
    main.appendChild(nav);
    main.classList.add("has-toc");
    var queued = false;
    function update() {
      queued = false;
      var cur = -1;
      heads.forEach(function (h, i) {
        var sec = h.parentNode, vis = shown(sec);
        links[i].classList.toggle("off", !vis);
        if (vis && sec.getBoundingClientRect().top <= 130) cur = i;
      });
      if (cur === -1) cur = heads.findIndex ? heads.findIndex(function (h) { return shown(h.parentNode); }) : 0;
      // at the bottom of the page the last sections cannot reach the top: the last one shown is the one being read
      if (window.innerHeight + window.scrollY >= document.documentElement.scrollHeight - 4) {
        for (var k = heads.length - 1; k >= 0; k--) { if (shown(heads[k].parentNode)) { cur = k; break; } }
      }
      links.forEach(function (a, i) {
        if (i === cur) a.setAttribute("aria-current", "location"); else a.removeAttribute("aria-current");
      });
    }
    function queue() { if (!queued) { queued = true; requestAnimationFrame(update); } }
    window.addEventListener("scroll", queue, { passive: true });
    window.addEventListener("resize", queue);
    window.addEventListener("tabchange", queue);
    update();
    return update;
  }

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
      var scope = a.closest("[data-gal-scope]") || a.closest(".gal");
      list = Array.prototype.slice.call(scope.querySelectorAll(".gal-item")).filter(function (x) { return x === a || shown(x); });
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
