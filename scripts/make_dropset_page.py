"""Render data/dropsets.json as a single-file review page: sets with their entries, filterable.

Usage: py -3 make_dropset_page.py <output.html>
"""
import collections
import json
import os
import sys

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")

SET_FIELDS = ["key", "string_key", "kind", "roll_type", "roll_count", "total_rate_pct", "used"]
ENTRY_FIELDS = ["result_label", "target", "target_name", "share_pct", "weight_pct", "min", "max", "enchant", "condition"]

TEMPLATE = r"""<title>Master Looter Drop Sets</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow+Condensed:wght@500;600&family=IBM+Plex+Sans:wght@400;500;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
:root{
  --bg:#EEF0F2; --panel:#F7F8F9; --panel2:#E4E7EA; --ink:#1B2024; --ink2:#4B5259; --mute:#7A8188;
  --line:#CFD4D9; --accent:#A8232B; --accent-ink:#FFFFFF; --tag:#DCE6EA; --tag-ink:#26424C;
  --class:#F3D9DB; --class-ink:#6E141A; --bar:#B8C7CE; --focus:#2F6F8F;
  --sans:"IBM Plex Sans",system-ui,-apple-system,"Segoe UI",sans-serif;
  --mono:"IBM Plex Mono",ui-monospace,Consolas,monospace;
  --disp:"Barlow Condensed","Arial Narrow",sans-serif;
}
@media (prefers-color-scheme: dark){
  :root:not([data-theme="light"]){
    --bg:#15181B; --panel:#1C2024; --panel2:#262B30; --ink:#E6E4DF; --ink2:#B4B8BC; --mute:#7F868C;
    --line:#333A40; --accent:#D9464F; --accent-ink:#1A0A0B; --tag:#243239; --tag-ink:#B9D3DC;
    --class:#4A1C20; --class-ink:#F2C7CA; --bar:#3B4A52; --focus:#7FB3CE;
  }
}
:root[data-theme="dark"]{
  --bg:#15181B; --panel:#1C2024; --panel2:#262B30; --ink:#E6E4DF; --ink2:#B4B8BC; --mute:#7F868C;
  --line:#333A40; --accent:#D9464F; --accent-ink:#1A0A0B; --tag:#243239; --tag-ink:#B9D3DC;
  --class:#4A1C20; --class-ink:#F2C7CA; --bar:#3B4A52; --focus:#7FB3CE;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 var(--sans)}
header{padding:22px 28px 14px;border-bottom:1px solid var(--line);display:flex;align-items:baseline;gap:20px;flex-wrap:wrap}
h1{font:600 34px/1 var(--disp);letter-spacing:.01em;margin:0;text-transform:uppercase}
h1 span{color:var(--accent)}
.meta{color:var(--ink2);font-size:13px}
.meta b{color:var(--ink);font-variant-numeric:tabular-nums}
main{padding:14px 28px 40px;display:grid;gap:14px}
.controls{display:grid;grid-template-columns:minmax(240px,2fr) repeat(3,minmax(150px,1fr)) auto;gap:10px;align-items:end}
label{display:grid;gap:4px;font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--mute)}
input,select,button{font:inherit;color:var(--ink);background:var(--panel);border:1px solid var(--line);border-radius:4px;padding:7px 9px}
input:focus,select:focus,button:focus{outline:2px solid var(--focus);outline-offset:1px}
button{cursor:pointer}
button.primary{background:var(--accent);color:var(--accent-ink);border-color:var(--accent);font-weight:600}
.checks{display:flex;gap:16px;flex-wrap:wrap;font-size:13px;color:var(--ink2)}
.checks label{display:flex;align-items:center;gap:6px;text-transform:none;letter-spacing:0;font-size:13px;color:var(--ink2)}
.checks input{accent-color:var(--accent)}
.status{display:flex;justify-content:space-between;color:var(--ink2);font-size:13px}
.status b{color:var(--ink);font-variant-numeric:tabular-nums}
.wrap{overflow-x:auto;border:1px solid var(--line);border-radius:6px;background:var(--panel)}
table{border-collapse:collapse;width:100%;min-width:1000px}
thead th{position:sticky;top:0;background:var(--panel2);text-align:left;font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--mute);padding:9px 10px;border-bottom:1px solid var(--line);white-space:nowrap}
td{padding:6px 10px;border-bottom:1px solid var(--line);vertical-align:top}
tr.set td{background:var(--panel2);padding:9px 10px;border-top:2px solid var(--line)}
tr.set .nm{font-family:var(--mono);font-size:13px;font-weight:500}
tr.set .kd{color:var(--ink2);font-size:12.5px}
.used{color:var(--ink2);font-size:12.5px;margin-top:4px;max-width:90ch}
.used b{color:var(--ink);font-variant-numeric:tabular-nums}
td.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
td.mono{font-family:var(--mono);font-size:12.5px}
td.dim{color:var(--ink2);font-size:13px}
.share{display:flex;align-items:center;gap:8px;justify-content:flex-end}
.share i{display:inline-block;height:8px;width:60px;background:var(--bar);border-radius:2px;position:relative;overflow:hidden}
.share i b{position:absolute;left:0;top:0;bottom:0;background:var(--accent)}
.chip{display:inline-block;padding:1px 7px;border-radius:3px;font-size:11.5px;background:var(--tag);color:var(--tag-ink);white-space:nowrap}
.chip.k{background:var(--class);color:var(--class-ink);font-weight:600}
mark{background:transparent;color:var(--accent);font-weight:600}
.more{display:flex;justify-content:center;padding:6px}
footer{padding:0 28px 30px;color:var(--mute);font-size:12px;max-width:80ch}
@media (max-width:900px){.controls{grid-template-columns:1fr 1fr}}
</style>
<header>
  <h1>Master <span>Looter</span> drop sets</h1>
  <div class="meta"><b>__NSETS__</b> drop sets &middot; <b>__NENTRIES__</b> entries &middot; Crimson Desert 2.02.00</div>
</header>
<main>
  <div class="controls">
    <label>Search set name, target key or item name<input id="q" type="search" placeholder="set, item, monster or gimmick name, e.g. Bear, Goblin, Honey, chest"></label>
    <label>Kind<select id="kind"><option value="designed">designed (DropSet_*)</option><option value="">all kinds</option>__KINDOPTS__</select></label>
    <label>Result type<select id="rtype"><option value="">Any</option>__TYPEOPTS__</select></label>
    <label>Min share %<input id="minshare" type="number" min="0" max="100" step="0.5" placeholder="0"></label>
    <button id="reset">Reset</button>
  </div>
  <div class="checks">
    <label><input type="checkbox" id="onlymatch" checked> show only matching entries inside a matched set</label>
    <label><input type="checkbox" id="hideknow"> hide knowledge and friendly-point entries</label>
  </div>
  <div class="status"><span>Showing <b id="shown">0</b> of <b id="match">0</b> matching sets</span><span id="note"></span></div>
  <div class="wrap">
    <table>
      <thead><tr><th>Set / target</th><th>Type</th><th>Item name</th><th>Share of roll</th><th>Weight</th><th>Qty</th><th>Enchant</th><th>Condition</th></tr></thead>
      <tbody id="body"></tbody>
    </table>
  </div>
  <div class="more"><button id="more" class="primary">Show 200 more sets</button></div>
</main>
<footer>Used-by lines come from the character reward lists and gimmick drop blocks found inside those records; NPC gear drops as the worn item itself (see item_sources.csv), not through a set. Share of roll is the entry's weight divided by the set's total weight: in every multi-entry set the weights add up exactly to the set total, so one roll picks one entry in proportion. Whether a set whose total is below 100% can also yield nothing is not decided by the table alone. Roll type and count are the stored bytes. Inline sets are script-generated single-target sets named after their content.</footer>
<script>
const SF = __SF__, EF = __EF__;
const SETS = __DATA__.map(a => { const s = {}; SF.forEach((f, i) => s[f] = a[i]); s.entries = a[SF.length].map(b => { const e = {}; EF.forEach((f, i) => e[f] = b[i]); return e; }); s._hay = (s.string_key + " " + s.entries.map(e => e.target + " " + e.target_name).join(" ") + " " + (s.used ? s.used[2].join(" ") + " " + s.used[3].join(" ") : "")).toLowerCase(); return s; });
const PAGE = 200; let view = [], shown = 0;
const $ = id => document.getElementById(id);
function esc(s){ return String(s == null ? "" : s).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c])); }
function hl(s, q){ s = esc(s); if (!q) return s; const i = s.toLowerCase().indexOf(q); return i < 0 ? s : s.slice(0, i) + "<mark>" + s.slice(i, i + q.length) + "</mark>" + s.slice(i + q.length); }
function entryMatch(e, q, rtype, minshare, hideknow){
  if (rtype && e.result_label !== rtype) return false;
  if (hideknow && (e.result_label === "knowledge" || e.result_label === "friendly points" || e.result_label === "knowledge group")) return false;
  if (minshare && (e.share_pct === "" || e.share_pct < minshare)) return false;
  if (q && !((e.target + " " + e.target_name).toLowerCase().includes(q))) return false;
  return true;
}
function apply(){
  const q = $("q").value.trim().toLowerCase(), kind = $("kind").value, rtype = $("rtype").value;
  const minshare = parseFloat($("minshare").value) || 0, hideknow = $("hideknow").checked;
  view = [];
  for (const s of SETS){
    if (kind && s.kind !== kind) continue;
    const nameHit = q && (s.string_key.toLowerCase().includes(q) || (s.used && (s.used[2].join(" ") + " " + s.used[3].join(" ")).toLowerCase().includes(q)));
    const es = s.entries.filter(e => entryMatch(e, nameHit ? "" : q, rtype, minshare, hideknow));
    if (!es.length) continue;
    view.push({ s, es: ($("onlymatch").checked ? es : s.entries), q });
  }
  shown = 0; $("body").innerHTML = ""; more();
  $("match").textContent = view.length.toLocaleString();
}
function qty(e){ return e.min === e.max ? e.min : e.min + "-" + e.max; }
function render(v){
  const s = v.s, q = v.q;
  let used = "";
  if (s.used) {
    const [nc, ng, cn, gn] = s.used;
    const parts = [];
    if (nc) parts.push('<b>' + nc + '</b> character' + (nc === 1 ? '' : 's') + ': ' + cn.map(x => hl(x, q)).join(', ') + (nc > cn.length ? ', &hellip;' : ''));
    if (ng) parts.push('<b>' + ng + '</b> gimmick' + (ng === 1 ? '' : 's') + ': ' + gn.map(x => hl(x, q)).join(', ') + (ng > gn.length ? ', &hellip;' : ''));
    used = '<div class="used">Used by ' + parts.join(' &middot; ') + '</div>';
  } else if (s.kind === "designed") {
    used = '<div class="used">No character or gimmick references this set (scripts, quests, stores or interactions may still use it).</div>';
  }
  let h = '<tr class="set"><td colspan="3"><span class="nm">' + hl(s.string_key, q) + '</span> <span class="kd">&middot; ' + esc(s.kind) + ' &middot; key ' + s.key + '</span>' + used + '</td>' +
    '<td colspan="5" class="kd">roll type ' + s.roll_type + ', count ' + s.roll_count + ' &middot; total weight ' + s.total_rate_pct + '% &middot; ' + s.entries.length + ' entr' + (s.entries.length === 1 ? 'y' : 'ies') + '</td></tr>';
  for (const e of v.es){
    const share = e.share_pct === "" ? "" : e.share_pct;
    h += '<tr><td class="mono">' + hl(e.target || ("#" + e.target_key), q) + '</td><td><span class="chip' + (e.result_label === "item" ? ' k' : '') + '">' + esc(e.result_label) + '</span></td>' +
      '<td>' + hl(e.target_name, q) + '</td><td class="num"><span class="share"><i><b style="width:' + Math.min(100, share || 0) + '%"></b></i>' + (share === "" ? "&ndash;" : share + "%") + '</span></td>' +
      '<td class="num">' + e.weight_pct + '%</td><td class="num">' + qty(e) + '</td><td class="num">' + (e.enchant == null ? "&ndash;" : "+" + e.enchant) + '</td><td class="dim">' + esc(e.condition) + '</td></tr>';
  }
  return h;
}
function more(){
  const next = view.slice(shown, shown + PAGE); shown += next.length;
  $("body").insertAdjacentHTML("beforeend", next.map(render).join(""));
  $("shown").textContent = shown.toLocaleString(); $("more").hidden = shown >= view.length;
}
["q","kind","rtype","minshare","onlymatch","hideknow"].forEach(id => $(id).addEventListener("input", apply));
$("more").addEventListener("click", more);
$("reset").addEventListener("click", () => { $("q").value = ""; $("kind").value = "designed"; $("rtype").value = ""; $("minshare").value = ""; $("onlymatch").checked = true; $("hideknow").checked = false; apply(); });
apply();
</script>
"""


def main():
    out = sys.argv[1]
    sets = json.load(open(os.path.join(DATA, "dropsets.json"), encoding="utf-8"))
    src_path = os.path.join(DATA, "sources.json")
    sources = json.load(open(src_path, encoding="utf-8")) if os.path.exists(src_path) else {}
    for s in sets:
        u = sources.get(str(s["key"]))
        s["used"] = [u["c"], u["g"], u["cn"], u["gn"]] if u else None
    data = [[s[f] for f in SET_FIELDS] + [[[e[f] for f in ENTRY_FIELDS] for e in s["entries"]]] for s in sets]
    kinds = collections.Counter(s["kind"] for s in sets)
    types = collections.Counter(e["result_label"] for s in sets for e in s["entries"])
    kindopts = "".join('<option value="%s">%s (%d)</option>' % (k, k, n) for k, n in kinds.most_common() if k != "designed")
    typeopts = "".join('<option value="%s">%s (%d)</option>' % (k, k, n) for k, n in types.most_common())
    html = (TEMPLATE.replace("__SF__", json.dumps(SET_FIELDS)).replace("__EF__", json.dumps(ENTRY_FIELDS))
            .replace("__DATA__", json.dumps(data, ensure_ascii=False, separators=(",", ":")))
            .replace("__NSETS__", "{:,}".format(len(sets))).replace("__NENTRIES__", "{:,}".format(sum(types.values())))
            .replace("__KINDOPTS__", kindopts).replace("__TYPEOPTS__", typeopts))
    assert "</script" not in json.dumps(data), "data would terminate the script block"
    with open(out, "w", encoding="utf-8") as f:
        f.write(html)
    print("wrote", out, len(html), "bytes")


if __name__ == "__main__":
    main()
