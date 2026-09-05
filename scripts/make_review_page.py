"""Render data/items_tagged.json as a single-file review page (filter, search, expand rows).

Usage: py -3 make_review_page.py <output.html>
"""
import collections
import json
import os
import sys

DATA = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "data")

FIELDS = ["key", "string_key", "name", "klass", "tags", "item_type_label", "equip_type", "tier",
          "max_stack", "value_copper", "leaf_groups", "root_groups", "desc", "important", "no_sell", "wild",
          "src_chars", "src_gims", "src_worn", "best_share", "src_c_text", "src_g_text", "src_e_text", "designed_sets"]

TEMPLATE = r"""<title>Master Looter Item Tags</title>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow+Condensed:wght@500;600&family=IBM+Plex+Sans:wght@400;500;600&family=IBM+Plex+Mono:wght@400;500&display=swap">
<style>
:root{
  --bg:#EEF0F2; --panel:#F7F8F9; --panel2:#E4E7EA; --ink:#1B2024; --ink2:#4B5259; --mute:#7A8188;
  --line:#CFD4D9; --accent:#A8232B; --accent-ink:#FFFFFF; --tag:#DCE6EA; --tag-ink:#26424C;
  --class:#F3D9DB; --class-ink:#6E141A; --warn:#8A5A00; --focus:#2F6F8F;
  --sans:"IBM Plex Sans",system-ui,-apple-system,"Segoe UI",sans-serif;
  --mono:"IBM Plex Mono",ui-monospace,Consolas,monospace;
  --disp:"Barlow Condensed","Arial Narrow",sans-serif;
}
@media (prefers-color-scheme: dark){
  :root:not([data-theme="light"]){
    --bg:#15181B; --panel:#1C2024; --panel2:#262B30; --ink:#E6E4DF; --ink2:#B4B8BC; --mute:#7F868C;
    --line:#333A40; --accent:#D9464F; --accent-ink:#1A0A0B; --tag:#243239; --tag-ink:#B9D3DC;
    --class:#4A1C20; --class-ink:#F2C7CA; --warn:#E0A94A; --focus:#7FB3CE;
  }
}
:root[data-theme="dark"]{
  --bg:#15181B; --panel:#1C2024; --panel2:#262B30; --ink:#E6E4DF; --ink2:#B4B8BC; --mute:#7F868C;
  --line:#333A40; --accent:#D9464F; --accent-ink:#1A0A0B; --tag:#243239; --tag-ink:#B9D3DC;
  --class:#4A1C20; --class-ink:#F2C7CA; --warn:#E0A94A; --focus:#7FB3CE;
}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:14px/1.45 var(--sans);}
header{padding:22px 28px 14px;border-bottom:1px solid var(--line);display:flex;align-items:baseline;gap:20px;flex-wrap:wrap}
h1{font:600 34px/1 var(--disp);letter-spacing:.01em;margin:0;text-transform:uppercase}
h1 span{color:var(--accent)}
.meta{color:var(--ink2);font-size:13px}
.meta b{color:var(--ink);font-variant-numeric:tabular-nums}
main{padding:14px 28px 40px;display:grid;gap:14px}
.controls{display:grid;grid-template-columns:minmax(220px,2fr) repeat(3,minmax(140px,1fr)) auto;gap:10px;align-items:end}
label{display:grid;gap:4px;font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--mute)}
input,select,button{font:inherit;color:var(--ink);background:var(--panel);border:1px solid var(--line);border-radius:4px;padding:7px 9px}
input:focus,select:focus,button:focus{outline:2px solid var(--focus);outline-offset:1px}
button{cursor:pointer}
button.primary{background:var(--accent);color:var(--accent-ink);border-color:var(--accent);font-weight:600}
.checks{display:flex;gap:14px;flex-wrap:wrap;align-items:center;font-size:13px;color:var(--ink2)}
.checks label{display:flex;align-items:center;gap:6px;text-transform:none;letter-spacing:0;font-size:13px;color:var(--ink2)}
.checks input{accent-color:var(--accent)}
.classes{display:flex;flex-wrap:wrap;gap:6px}
.classes button{padding:4px 9px;border-radius:999px;background:var(--panel2);border-color:transparent;font-size:12px;display:flex;gap:6px;align-items:center}
.classes button b{font-variant-numeric:tabular-nums;color:var(--ink2);font-weight:500}
.classes button.on{background:var(--accent);color:var(--accent-ink)}
.classes button.on b{color:var(--accent-ink)}
.status{display:flex;justify-content:space-between;align-items:center;color:var(--ink2);font-size:13px}
.status b{font-variant-numeric:tabular-nums;color:var(--ink)}
.wrap{overflow-x:auto;border:1px solid var(--line);border-radius:6px;background:var(--panel)}
table{border-collapse:collapse;width:100%;min-width:1100px}
thead th{position:sticky;top:0;background:var(--panel2);text-align:left;font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--mute);padding:9px 10px;border-bottom:1px solid var(--line);cursor:pointer;white-space:nowrap}
thead th.sorted{color:var(--ink)}
tbody td{padding:7px 10px;border-bottom:1px solid var(--line);vertical-align:top}
tbody tr.row{cursor:pointer}
tbody tr.row:hover td{background:var(--panel2)}
td.num{text-align:right;font-variant-numeric:tabular-nums;white-space:nowrap}
td.mono{font-family:var(--mono);font-size:12.5px;white-space:nowrap}
td.name{font-weight:500;min-width:180px}
td.dim{color:var(--ink2);font-size:13px}
.chip{display:inline-block;padding:1px 7px;border-radius:3px;font-size:11.5px;line-height:1.5;margin:1px 3px 1px 0;background:var(--tag);color:var(--tag-ink);white-space:nowrap}
.chip.k{background:var(--class);color:var(--class-ink);font-weight:600}
.chip.attr{background:transparent;border:1px solid var(--line);color:var(--ink2)}
.best{color:var(--accent);font-weight:600;white-space:nowrap}
tr.detail td{background:var(--panel2);padding:12px 16px 14px 44px;font-size:13px}
.det{display:grid;grid-template-columns:repeat(auto-fit,minmax(260px,1fr));gap:10px 24px}
.det div{max-width:70ch}
.det dt{font-size:11px;letter-spacing:.08em;text-transform:uppercase;color:var(--mute);margin-bottom:2px}
.det dd{margin:0}
.more{display:flex;justify-content:center;padding:6px}
footer{padding:0 28px 30px;color:var(--mute);font-size:12px;max-width:80ch}
@media (max-width:900px){.controls{grid-template-columns:1fr 1fr}}
@media (prefers-reduced-motion:no-preference){tbody tr.row td{transition:background .12s}}
</style>
<header>
  <h1>Master <span>Looter</span> item tags</h1>
  <div class="meta"><b>__N__</b> items from iteminfo.staticinfobody, Crimson Desert 2.01.00 &middot; <b>__NCLASS__</b> classes &middot; <b>__NTAGS__</b> tags &middot; <b>__NSRC__</b> items with a known drop source</div>
</header>
<main>
  <div class="controls">
    <label>Search name, key, tags, groups<input id="q" type="search" placeholder="e.g. arrow, Kliff, no-sell, Damaged"></label>
    <label>Class<select id="cls"><option value="">All classes</option>__CLASSOPTS__</select></label>
    <label>Has tag<select id="tag"><option value="">Any tag</option>__TAGOPTS__</select></label>
    <label>Tier<select id="tier"><option value="">Any tier</option><option>0</option><option>1</option><option>2</option><option>3</option><option>4</option><option>5</option></select></label>
    <button id="reset">Reset</button>
  </div>
  <div class="checks">
    <label><input type="checkbox" id="hidedev" checked> hide dev and test items</label>
    <label><input type="checkbox" id="onlyimp"> important only</label>
    <label><input type="checkbox" id="onlysell"> sellable only</label>
    <label><input type="checkbox" id="onlystack"> stackable only</label>
    <label><input type="checkbox" id="onlysrc"> has a drop source (character, gimmick or worn gear)</label>
  </div>
  <div class="classes" id="classes">__CLASSCHIPS__</div>
  <div class="status"><span>Showing <b id="shown">0</b> of <b id="match">0</b> matching rows</span><span id="sortnote"></span></div>
  <div class="wrap">
    <table>
      <thead><tr>
        <th data-k="key">Key</th><th data-k="string_key">String key</th><th data-k="name">Name</th>
        <th data-k="klass">Class</th><th>Tags</th><th data-k="item_type_label">Type byte</th>
        <th data-k="tier">Tier</th><th data-k="max_stack">Stack</th><th data-k="value_copper">Copper</th><th data-k="src_total">Drops from</th><th data-k="leaf_groups">Leaf groups</th>
      </tr></thead>
      <tbody id="body"></tbody>
    </table>
  </div>
  <div class="more"><button id="more" class="primary">Show 500 more</button></div>
</main>
<footer>Drops from counts the characters and gimmicks whose drop sets contain the item, plus the NPCs that wear it (gear drops as the worn item at its death-drop chance); the percentage is the best single chance found. Gatherables such as herbs and ore are placed items rather than drops, so they show no source. Class is the first matching tag in a fixed priority list; every other tag stays in the Tags column. Tier, stack, copper value and flags come straight from the item record. Click a row for the description, all groups and prices. Copper is the sell price in the item's price list for currency key 1.</footer>
<script>
const F = __FIELDS__;
const RAW = __DATA__;
const ROWS = RAW.map(a => { const o = {}; F.forEach((f, i) => o[f] = a[i]); o._tags = o.tags.split(" "); o.src_total = o.src_chars + o.src_gims + o.src_worn; o._hay = (o.string_key + " " + o.name + " " + o.tags + " " + o.leaf_groups + " " + o.item_type_label + " " + o.equip_type + " " + o.src_c_text + " " + o.src_g_text + " " + o.src_e_text).toLowerCase(); return o; });
function srcSummary(o){
  const p = [];
  if (o.src_chars) p.push(o.src_chars + " character" + (o.src_chars === 1 ? "" : "s"));
  if (o.src_gims) p.push(o.src_gims + " gimmick" + (o.src_gims === 1 ? "" : "s"));
  if (o.src_worn) p.push("worn by " + o.src_worn);
  if (!p.length) return "&ndash;";
  return esc(p.join(" / ")) + (o.best_share !== "" ? ' <span class="best">up to ' + o.best_share + '%</span>' : '');
}
const ATTR = new Set(["stackable","important","no-sell","no-discard","housing-only","wild","preorder","extractable","use-immediately","hidden","knowledge","docking","blocked"]);
const PAGE = 500;
let view = [], shown = 0, sortKey = "key", sortDir = 1, cls = "";
const $ = id => document.getElementById(id);
function esc(s){ return String(s == null ? "" : s).replace(/[&<>"]/g, c => ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c])); }
function chips(o){
  return o._tags.filter(t => t !== o.klass && !t.startsWith("tier-")).map(t => '<span class="chip' + (ATTR.has(t) ? ' attr' : '') + '">' + esc(t) + '</span>').join("");
}
function rowHtml(o){
  return '<tr class="row" data-key="' + o.key + '"><td class="num">' + o.key + '</td><td class="mono">' + esc(o.string_key) + '</td><td class="name">' + esc(o.name || "(no English name)") +
    '</td><td><span class="chip k">' + esc(o.klass) + '</span></td><td>' + chips(o) + '</td><td class="dim">' + esc(o.item_type_label) + '</td><td class="num">' + o.tier +
    '</td><td class="num">' + (o.max_stack < 0 ? "&infin;" : o.max_stack) + '</td><td class="num">' + (o.value_copper === "" ? "&ndash;" : o.value_copper) + '</td><td class="dim">' + srcSummary(o) + '</td><td class="dim">' + esc(o.leaf_groups) + '</td></tr>';
}
function detailHtml(o){
  return '<tr class="detail"><td colspan="11"><dl class="det">' +
    '<div><dt>Description</dt><dd>' + (esc(o.desc).replace(/&lt;br\/?&gt;/g, " ") || "&ndash;") + '</dd></div>' +
    '<div><dt>All tags</dt><dd>' + o._tags.map(t => '<span class="chip' + (t === o.klass ? ' k' : ATTR.has(t) ? ' attr' : '') + '">' + esc(t) + '</span>').join("") + '</dd></div>' +
    '<div><dt>Leaf groups</dt><dd>' + esc(o.leaf_groups) + '</dd><dt style="margin-top:8px">Root groups</dt><dd>' + esc(o.root_groups) + '</dd></div>' +
    '<div><dt>Dropped by characters (share of their set roll)</dt><dd>' + (esc(o.src_c_text) || "&ndash;") + '</dd></div>' +
    '<div><dt>From gimmicks (chests, barrels, trees)</dt><dd>' + (esc(o.src_g_text) || "&ndash;") + '</dd></div>' +
    '<div><dt>Worn by NPCs (death-drop chance)</dt><dd>' + (esc(o.src_e_text) || "&ndash;") + '</dd></div>' +
    '<div><dt>Designed drop sets</dt><dd>' + (esc(o.designed_sets) || "&ndash;") + '</dd></div>' +
    '<div><dt>Record</dt><dd class="mono">key ' + o.key + ' &middot; type byte ' + esc(o.item_type_label) + (o.equip_type ? ' &middot; equip ' + esc(o.equip_type) : '') + ' &middot; important ' + o.important + ' &middot; no_sell ' + o.no_sell + ' &middot; wild ' + o.wild + '</dd></div>' +
    '</dl></td></tr>';
}
function apply(){
  const q = $("q").value.trim().toLowerCase(), tag = $("tag").value, tier = $("tier").value;
  const hidedev = $("hidedev").checked, onlyimp = $("onlyimp").checked, onlysell = $("onlysell").checked, onlystack = $("onlystack").checked, onlysrc = $("onlysrc").checked;
  view = ROWS.filter(o => (!cls || o.klass === cls) && (!tag || o._tags.includes(tag)) && (tier === "" || String(o.tier) === tier) &&
    (!hidedev || o.klass !== "dev") && (!onlyimp || o.important) && (!onlysell || !o.no_sell) && (!onlystack || o.max_stack !== 1) && (!onlysrc || o.src_total > 0) && (!q || o._hay.includes(q)));
  view.sort((a, b) => { let x = a[sortKey], y = b[sortKey]; if (x === "") x = -1; if (y === "") y = -1; return (x < y ? -1 : x > y ? 1 : 0) * sortDir || a.key - b.key; });
  shown = 0; $("body").innerHTML = ""; more();
  $("match").textContent = view.length.toLocaleString();
  document.querySelectorAll("#classes button").forEach(b => b.classList.toggle("on", b.dataset.c === cls));
  document.querySelectorAll("thead th").forEach(th => th.classList.toggle("sorted", th.dataset.k === sortKey));
  $("sortnote").textContent = "sorted by " + sortKey + (sortDir < 0 ? " (desc)" : "");
}
function more(){
  const next = view.slice(shown, shown + PAGE); shown += next.length;
  $("body").insertAdjacentHTML("beforeend", next.map(rowHtml).join(""));
  $("shown").textContent = shown.toLocaleString();
  $("more").hidden = shown >= view.length;
}
["q","tag","tier","hidedev","onlyimp","onlysell","onlystack","onlysrc"].forEach(id => $(id).addEventListener("input", apply));
$("cls").addEventListener("input", e => { cls = e.target.value; apply(); });
$("classes").addEventListener("click", e => { const b = e.target.closest("button"); if (!b) return; cls = cls === b.dataset.c ? "" : b.dataset.c; $("cls").value = cls; apply(); });
$("more").addEventListener("click", more);
$("reset").addEventListener("click", () => { $("q").value = ""; $("tag").value = ""; $("tier").value = ""; $("cls").value = ""; cls = ""; $("hidedev").checked = true; $("onlyimp").checked = $("onlysell").checked = $("onlystack").checked = $("onlysrc").checked = false; sortKey = "key"; sortDir = 1; apply(); });
document.querySelector("thead").addEventListener("click", e => { const th = e.target.closest("th"); if (!th || !th.dataset.k) return; if (sortKey === th.dataset.k) sortDir = -sortDir; else { sortKey = th.dataset.k; sortDir = 1; } apply(); });
$("body").addEventListener("click", e => {
  const tr = e.target.closest("tr.row"); if (!tr) return;
  const nx = tr.nextElementSibling;
  if (nx && nx.classList.contains("detail")) { nx.remove(); return; }
  const o = ROWS.find(r => r.key == tr.dataset.key); tr.insertAdjacentHTML("afterend", detailHtml(o));
});
apply();
</script>
"""


def main():
    out = sys.argv[1]
    rows = json.load(open(os.path.join(DATA, "items_tagged.json"), encoding="utf-8"))
    import csv
    src = {int(r["key"]): r for r in csv.DictReader(open(os.path.join(DATA, "item_sources.csv"), encoding="utf-8-sig"))}
    dsets = {int(r["key"]): r for r in csv.DictReader(open(os.path.join(DATA, "item_drop_sets.csv"), encoding="utf-8-sig"))}
    n_src = 0
    for r in rows:
        x = src.get(r["key"]); d = dsets.get(r["key"])
        r["src_chars"] = int(x["n_characters_via_sets"]) if x else 0
        r["src_gims"] = int(x["n_gimmicks_via_sets"]) if x else 0
        r["src_worn"] = int(x["n_equipped_by"]) if x else 0
        best = ""
        if x:
            cands = [float(v) for v in (x["best_share_pct"], x["max_equip_drop_pct"]) if v not in ("", None)]
            best = round(max(cands), 2) if cands else ""
        r["best_share"] = best
        r["src_c_text"] = x["characters"] if x else ""
        r["src_g_text"] = x["gimmicks"] if x else ""
        r["src_e_text"] = x["equipped_by"] if x else ""
        r["designed_sets"] = d["designed_sets"] if d else ""
        if r["src_chars"] or r["src_gims"] or r["src_worn"]:
            n_src += 1
    data = [[r[f] for f in FIELDS] for r in rows]
    classes = collections.Counter(r["klass"] for r in rows)
    tags = collections.Counter(t for r in rows for t in r["tags"].split())
    classopts = "".join('<option value="%s">%s (%d)</option>' % (k, k, n) for k, n in classes.most_common())
    tagopts = "".join('<option value="%s">%s (%d)</option>' % (k, k, n) for k, n in sorted(tags.items()))
    chips = "".join('<button data-c="%s">%s <b>%d</b></button>' % (k, k, n) for k, n in classes.most_common(28))
    html = (TEMPLATE.replace("__FIELDS__", json.dumps(FIELDS)).replace("__DATA__", json.dumps(data, ensure_ascii=False, separators=(",", ":")))
            .replace("__N__", "{:,}".format(len(rows))).replace("__NCLASS__", str(len(classes))).replace("__NTAGS__", str(len(tags))).replace("__NSRC__", "{:,}".format(n_src))
            .replace("__CLASSOPTS__", classopts).replace("__TAGOPTS__", tagopts).replace("__CLASSCHIPS__", chips))
    with open(out, "w", encoding="utf-8") as f:
        f.write(html)
    print("wrote", out, len(html), "bytes")


if __name__ == "__main__":
    main()
