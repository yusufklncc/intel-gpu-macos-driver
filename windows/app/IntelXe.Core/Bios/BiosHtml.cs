// SPDX-License-Identifier: MIT
// The BIOS steps as one printable HTML page, to open on a phone or print before going into the firmware setup: no
// scripts, no external files (styles inline, a CSP that loads nothing), light and dark, narrow screens and paper.
// Every text from Windows (the board's name, controller names) is escaped.
using System.Globalization;
using System.Text;

namespace IntelXe.Core.Bios;

public static class BiosHtml
{
    const string Csp = "default-src 'none'; style-src 'unsafe-inline'";

    /// Escapes the five characters HTML gives meaning to; the rest stays as UTF-8 text (Turkish letters too).
    public static string E(string s) => s.Replace("&", "&amp;").Replace("<", "&lt;").Replace(">", "&gt;").Replace("\"", "&quot;").Replace("'", "&#39;");

    /// The page for these steps, in "en" or "tr", with the board's menu names (vendor id).
    public static string Render(BiosData data, IReadOnlyList<BiosStep> steps, PcFacts? facts, string vendorId, string lang, DateTime made)
    {
        string T(string key) => data.PageText(key, lang);
        var vendor = BiosSteps.Vendor(data, vendorId);
        var sb = new StringBuilder();
        sb.Append("<!doctype html>\n<html lang=\"").Append(lang).Append("\">\n<head>\n<meta charset=\"utf-8\">\n")
          .Append("<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n")
          .Append("<meta http-equiv=\"Content-Security-Policy\" content=\"").Append(Csp).Append("\">\n")
          .Append("<title>").Append(E(T("title"))).Append("</title>\n<style>\n").Append(Style).Append("</style>\n</head>\n<body>\n<main>\n");

        sb.Append("<h1>").Append(E(T("title"))).Append("</h1>\n<p class=\"lead\">").Append(E(T("subtitle"))).Append("</p>\n");

        // the PC and the keys
        sb.Append("<dl class=\"facts\">\n");
        if (facts is not null)
        {
            var board = $"{facts.BoardMaker} {facts.BoardModel}".Trim();
            if (board.Length > 0) sb.Append("<dt>").Append(E(T("board"))).Append("</dt><dd>").Append(E(board)).Append("</dd>\n");
            var cpu = CpuInfo.CleanName(facts.CpuName);
            if (cpu.Length > 0) sb.Append("<dt>").Append(E(T("cpu"))).Append("</dt><dd>").Append(E(cpu)).Append("</dd>\n");
        }
        sb.Append("<dt>").Append(E(T("menus"))).Append("</dt><dd>").Append(E(vendor.Name.Length > 0 ? vendor.Name : T("menus-generic"))).Append("</dd>\n");
        sb.Append("<dt>").Append(E(T("setup-key"))).Append("</dt><dd><kbd>").Append(E(vendor.SetupKey)).Append("</kbd></dd>\n");
        if (vendor.AdvancedKey.Length > 0) sb.Append("<dt>").Append(E(T("advanced-key"))).Append("</dt><dd><kbd>").Append(E(vendor.AdvancedKey)).Append("</kbd></dd>\n");
        sb.Append("<dt>").Append(E(T("save-key"))).Append("</dt><dd><kbd>").Append(E(vendor.SaveKey)).Append("</kbd></dd>\n");
        sb.Append("<dt>").Append(E(T("boot-key"))).Append("</dt><dd><kbd>").Append(E(vendor.BootMenuKey)).Append("</kbd></dd>\n");
        sb.Append("</dl>\n<p class=\"note\">").Append(E(vendor.Note.In(lang))).Append("</p>\n");

        var (todo, done, unknown) = BiosSteps.Count(steps);
        sb.Append("<p class=\"summary\">").Append(E(string.Format(CultureInfo.InvariantCulture, T("summary"), todo, done, unknown))).Append("</p>\n");

        int n = 0;
        foreach (var group in data.Groups)
        {
            var mine = steps.Where(s => s.Item.Group == group.Id).ToList();
            if (mine.Count == 0) continue;
            sb.Append("<section>\n<h2>").Append(E(group.Title.In(lang))).Append("</h2>\n<p class=\"caption\">").Append(E(group.Caption.In(lang))).Append("</p>\n<ol start=\"").Append(n + 1).Append("\">\n");
            foreach (var s in mine)
            {
                n++;
                var item = s.Item;
                string state = s.State switch { BiosState.Done => "done", BiosState.ToDo => "todo", _ => "unknown" };
                string level = item.Level switch { BiosLevel.Required => "required", BiosLevel.Recommended => "recommended", _ => "optional" };
                sb.Append("<li class=\"item ").Append(state).Append("\">\n<div class=\"head\"><span class=\"chip ").Append(state).Append("\">")
                  .Append(E(T("state-" + state))).Append("</span> <strong>").Append(E(item.Title.In(lang))).Append("</strong> <span class=\"level\">")
                  .Append(E(T("level-" + level))).Append("</span></div>\n");
                sb.Append("<div class=\"want\">&rarr; ").Append(E(item.Want.In(lang))).Append("</div>\n");
                var path = item.PathFor(vendor.Id, lang);
                if (path.Length > 0) sb.Append("<div><span class=\"k\">").Append(E(T("where"))).Append(":</span> <code>").Append(E(path)).Append("</code></div>\n");
                sb.Append("<div><span class=\"k\">").Append(E(T("seen"))).Append(":</span> ").Append(E(s.Seen.In(data, lang))).Append("</div>\n");
                sb.Append("<p>").Append(E(item.Why.In(lang))).Append("</p>\n");
                if (item.Missing is { } m) sb.Append("<p class=\"missing\"><span class=\"k\">").Append(E(T("missing"))).Append(":</span> ").Append(E(m.In(lang))).Append("</p>\n");
                var links = item.Sources.Select(id => data.Sources.TryGetValue(BiosSteps.SourceFor(id, facts), out var src) ? src : null).Where(x => x is not null).ToList();
                if (links.Count > 0)
                {
                    sb.Append("<p class=\"sources\"><span class=\"k\">").Append(E(T("sources"))).Append(":</span> ");
                    sb.Append(string.Join(" · ", links.Select(src => "<a href=\"" + E(src!.Url) + "\" title=\"" + E(src.Title) + "\">" + E(src.Short.In(lang)) + "</a>")));
                    sb.Append("</p>\n");
                }
                sb.Append("</li>\n");
            }
            sb.Append("</ol>\n</section>\n");
        }
        sb.Append("<footer>").Append(E(string.Format(CultureInfo.InvariantCulture, T("footer"), made.ToString("yyyy-MM-dd HH:mm", CultureInfo.InvariantCulture)))).Append("</footer>\n");
        sb.Append("</main>\n</body>\n</html>\n");
        return sb.ToString();
    }

    const string Style = """
:root { color-scheme: light dark; --bg: #ffffff; --fg: #1d1d1f; --muted: #6e6e73; --line: #d2d2d7; --card: #f5f5f7;
  --done: #1e8e3e; --todo: #c25100; --unknown: #6e6e73; --accent: #007aff; }
@media (prefers-color-scheme: dark) { :root { --bg: #1c1c1e; --fg: #f5f5f7; --muted: #98989d; --line: #3a3a3c; --card: #2c2c2e;
  --done: #30d158; --todo: #ff9f0a; --unknown: #98989d; --accent: #0a84ff; } }
* { box-sizing: border-box; }
body { margin: 0; background: var(--bg); color: var(--fg); font: 16px/1.45 "Segoe UI", system-ui, -apple-system, sans-serif; }
main { max-width: 760px; margin: 0 auto; padding: 20px 16px 40px; }
h1 { font-size: 1.5em; margin: 0 0 6px; }
h2 { font-size: 1.15em; margin: 26px 0 4px; }
.lead, .caption, .note, footer { color: var(--muted); }
.caption { margin: 0 0 8px; }
dl.facts { display: grid; grid-template-columns: max-content minmax(0, 1fr); gap: 4px 14px; margin: 14px 0 6px; padding: 12px 14px;
  background: var(--card); border: 1px solid var(--line); border-radius: 10px; }
dl.facts dt { color: var(--muted); }
dl.facts dd { margin: 0; overflow-wrap: anywhere; }
kbd { font: 0.92em ui-monospace, "Cascadia Mono", Consolas, monospace; border: 1px solid var(--line); border-bottom-width: 2px;
  border-radius: 5px; padding: 0 5px; }
.summary { font-weight: 600; }
ol { padding-left: 0; list-style: none; counter-reset: none; margin: 0; }
li.item { border: 1px solid var(--line); border-left: 5px solid var(--unknown); border-radius: 10px; padding: 10px 14px; margin: 10px 0;
  break-inside: avoid; }
li.item.done { border-left-color: var(--done); }
li.item.todo { border-left-color: var(--todo); }
li.item p { margin: 6px 0 0; }
.head { display: flex; flex-wrap: wrap; gap: 6px 8px; align-items: baseline; }
.chip { font-size: 0.8em; font-weight: 700; text-transform: uppercase; letter-spacing: 0.03em; padding: 1px 8px; border-radius: 10px;
  color: #fff; background: var(--unknown); }
.chip.done { background: var(--done); }
.chip.todo { background: var(--todo); }
.level { color: var(--muted); font-size: 0.85em; }
.want { font-weight: 600; margin: 4px 0; }
.k { color: var(--muted); }
code { font: 0.9em ui-monospace, "Cascadia Mono", Consolas, monospace; overflow-wrap: anywhere; }
.missing, .sources { font-size: 0.92em; }
a { color: var(--accent); overflow-wrap: anywhere; }
footer { margin-top: 28px; font-size: 0.85em; }
@media print { body { background: #fff; color: #000; font-size: 11pt; } main { max-width: none; padding: 0; }
  li.item { border-color: #999; } a { color: #000; text-decoration: none; } .chip { color: #000; background: none; border: 1px solid #000; } }

""";
}
