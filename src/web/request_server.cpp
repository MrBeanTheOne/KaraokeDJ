// httplib pulls winsock2 — it must come before anything that includes
// windows.h (WIN32_LEAN_AND_MEAN keeps winsock.h out of windows.h itself).
#include "httplib.h"

#include "web/request_server.h"

#include <iphlpapi.h>
#include <ws2tcpip.h>

#include <chrono>
#include <map>

#include "library/db.h"

// ---------------------------------------------------------------- phone page
// One self-contained mobile page, dark to match the app. Vanilla JS: name box
// (remembered per phone), debounced search, request buttons.
static const char kPage[] = R"HTML(<!doctype html>
<html lang="en"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Song Requests</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; background: #0a0a0c; color: #ececef;
         font: 16px/1.4 system-ui, "Segoe UI", sans-serif;
         padding: max(12px, env(safe-area-inset-top)) 14px 24px; }
  h1 { font-size: 19px; letter-spacing: .12em; color: #d22c36; margin: 6px 0 2px; }
  .sub { color: #94949d; font-size: 12px; margin: 0 0 14px; }
  label { display: block; color: #94949d; font-size: 11px; letter-spacing: .08em;
          margin: 12px 0 4px; }
  input { width: 100%; padding: 12px; font-size: 16px; color: #ececef;
          background: #141416; border: 1px solid #2a2a2f; border-radius: 9px;
          outline: none; }
  input:focus { border-color: #d22c36; }
  #list { margin-top: 12px; }
  .row { display: flex; align-items: center; gap: 10px; padding: 10px 4px;
         border-bottom: 1px solid #1c1c20; }
  .meta { flex: 1; min-width: 0; }
  .t { font-weight: 600; white-space: nowrap; overflow: hidden;
       text-overflow: ellipsis; }
  .a { color: #94949d; font-size: 13px; white-space: nowrap; overflow: hidden;
       text-overflow: ellipsis; }
  button { flex: none; padding: 10px 14px; font-size: 13px; font-weight: 700;
           letter-spacing: .06em; color: #fff; background: #d22c36;
           border: 0; border-radius: 9px; }
  button:disabled { background: #2a2a2f; color: #94949d; }
  #toast { position: fixed; left: 14px; right: 14px;
           bottom: max(16px, env(safe-area-inset-bottom)); padding: 13px;
           text-align: center; font-weight: 600; border-radius: 10px;
           background: #14532d; color: #b9f6ca; opacity: 0;
           transition: opacity .25s; pointer-events: none; }
  #toast.err { background: #58151c; color: #ffb4ab; }
  #toast.show { opacity: 1; }
  .hint { color: #94949d; font-size: 13px; text-align: center; margin-top: 26px; }
  #gate { position: fixed; inset: 0; z-index: 9; background: #0a0a0c;
          padding: 15vh 24px 0; }
  #gate button { width: 100%; margin-top: 14px; padding: 13px; }
  #gate[hidden] { display: none; }
</style></head><body>
<h1>KARAOKE DJ</h1>
<p class="sub">Search a song, tap REQUEST — the DJ adds you to the rotation.</p>
<label>YOUR NAME</label>
<input id="name" maxlength="40" placeholder="Who's singing?" autocomplete="off">
<label>FIND A SONG</label>
<input id="q" placeholder="Title or artist…" autocomplete="off">
<div id="list"><p class="hint">Type at least 2 letters to search.</p></div>
<div id="toast"></div>
<div id="gate" hidden>
  <h1>KARAOKE DJ</h1>
  <p class="sub">This party needs a password — ask the DJ.</p>
  <label>PASSWORD</label>
  <input id="pw" type="password" autocomplete="off">
  <button id="go">ENTER</button>
  <p id="gerr" class="hint"></p>
</div>
<script>
"use strict";
const $ = id => document.getElementById(id);
let PW = "";
try { PW = localStorage.getItem("kdj_pw") || ""; } catch (e) {}
function gate(show) { $("gate").hidden = !show; if (show) $("pw").focus(); }
async function ping() {
  try { gate(!(await fetch("/api/ping?pw=" + encodeURIComponent(PW))).ok); }
  catch (e) {}
}
ping();
$("go").addEventListener("click", async () => {
  PW = $("pw").value;
  try {
    if ((await fetch("/api/ping?pw=" + encodeURIComponent(PW))).ok) {
      try { localStorage.setItem("kdj_pw", PW); } catch (e) {}
      gate(false);
    } else {
      $("gerr").textContent = "Wrong password — try again.";
    }
  } catch (e) { $("gerr").textContent = "Couldn't reach the DJ app."; }
});
$("pw").addEventListener("keydown", ev => {
  if (ev.key === "Enter") $("go").click();
});
try { $("name").value = localStorage.getItem("kdj_name") || ""; } catch (e) {}
$("name").addEventListener("input", () => {
  try { localStorage.setItem("kdj_name", $("name").value); } catch (e) {}
});
let timer = 0, toastTimer = 0;
function toast(msg, err) {
  const t = $("toast");
  t.textContent = msg;
  t.className = (err ? "err " : "") + "show";
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = err ? "err" : ""; }, 2600);
}
function esc(s) {
  return s.replace(/[&<>"]/g, c =>
    ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
}
function fmt(ms) {
  const s = Math.round(ms / 1000);
  return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
}
$("q").addEventListener("input", () => {
  clearTimeout(timer);
  timer = setTimeout(search, 250);
});
async function search() {
  const q = $("q").value.trim();
  if (q.length < 2) {
    $("list").innerHTML = '<p class="hint">Type at least 2 letters to search.</p>';
    return;
  }
  try {
    const rs = await fetch("/api/search?q=" + encodeURIComponent(q) +
                           "&pw=" + encodeURIComponent(PW));
    if (rs.status === 401) { gate(true); return; }
    const rows = await rs.json();
    if (!rows.length) {
      $("list").innerHTML = '<p class="hint">No matches.</p>';
      return;
    }
    $("list").innerHTML = rows.map(r =>
      '<div class="row"><div class="meta"><div class="t">' + esc(r.t) +
      '</div><div class="a">' + esc(r.a || "—") +
      (r.d > 0 ? " · " + fmt(r.d) : "") +
      '</div></div><button data-id="' + r.id + '">REQUEST</button></div>').join("");
  } catch (e) {
    $("list").innerHTML = '<p class="hint">Search failed — is the DJ app running?</p>';
  }
}
$("list").addEventListener("click", async ev => {
  const b = ev.target.closest("button");
  if (!b) return;
  const name = $("name").value.trim();
  if (!name) { toast("Enter your name first!", true); $("name").focus(); return; }
  b.disabled = true;
  try {
    const rs = await fetch("/api/request", {
      method: "POST",
      body: new URLSearchParams({ id: b.dataset.id, singer: name, pw: PW }),
    });
    if (rs.status === 401) { gate(true); b.disabled = false; return; }
    const j = await rs.json();
    toast(j.msg || (rs.ok ? "Request sent!" : "Request failed"), !rs.ok);
    if (!rs.ok) b.disabled = false;
  } catch (e) {
    toast("Couldn't reach the DJ app.", true);
    b.disabled = false;
  }
});
</script></body></html>)HTML";

static const char kPageFr[] = R"HTML(<!doctype html>
<html lang="fr"><head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<title>Demandes de chansons</title>
<style>
  :root { color-scheme: dark; }
  * { box-sizing: border-box; }
  body { margin: 0; background: #0a0a0c; color: #ececef;
         font: 16px/1.4 system-ui, "Segoe UI", sans-serif;
         padding: max(12px, env(safe-area-inset-top)) 14px 24px; }
  h1 { font-size: 19px; letter-spacing: .12em; color: #d22c36; margin: 6px 0 2px; }
  .sub { color: #94949d; font-size: 12px; margin: 0 0 14px; }
  label { display: block; color: #94949d; font-size: 11px; letter-spacing: .08em;
          margin: 12px 0 4px; }
  input { width: 100%; padding: 12px; font-size: 16px; color: #ececef;
          background: #141416; border: 1px solid #2a2a2f; border-radius: 9px;
          outline: none; }
  input:focus { border-color: #d22c36; }
  #list { margin-top: 12px; }
  .row { display: flex; align-items: center; gap: 10px; padding: 10px 4px;
         border-bottom: 1px solid #1c1c20; }
  .meta { flex: 1; min-width: 0; }
  .t { font-weight: 600; white-space: nowrap; overflow: hidden;
       text-overflow: ellipsis; }
  .a { color: #94949d; font-size: 13px; white-space: nowrap; overflow: hidden;
       text-overflow: ellipsis; }
  button { flex: none; padding: 10px 14px; font-size: 13px; font-weight: 700;
           letter-spacing: .06em; color: #fff; background: #d22c36;
           border: 0; border-radius: 9px; }
  button:disabled { background: #2a2a2f; color: #94949d; }
  #toast { position: fixed; left: 14px; right: 14px;
           bottom: max(16px, env(safe-area-inset-bottom)); padding: 13px;
           text-align: center; font-weight: 600; border-radius: 10px;
           background: #14532d; color: #b9f6ca; opacity: 0;
           transition: opacity .25s; pointer-events: none; }
  #toast.err { background: #58151c; color: #ffb4ab; }
  #toast.show { opacity: 1; }
  .hint { color: #94949d; font-size: 13px; text-align: center; margin-top: 26px; }
  #gate { position: fixed; inset: 0; z-index: 9; background: #0a0a0c;
          padding: 15vh 24px 0; }
  #gate button { width: 100%; margin-top: 14px; padding: 13px; }
  #gate[hidden] { display: none; }
</style></head><body>
<h1>KARAOKE DJ</h1>
<p class="sub">Cherchez une chanson, touchez DEMANDER — le DJ vous ajoute à la rotation.</p>
<label>VOTRE NOM</label>
<input id="name" maxlength="40" placeholder="Qui chante ?" autocomplete="off">
<label>TROUVER UNE CHANSON</label>
<input id="q" placeholder="Titre ou artiste…" autocomplete="off">
<div id="list"><p class="hint">Tapez au moins 2 lettres pour chercher.</p></div>
<div id="toast"></div>
<div id="gate" hidden>
  <h1>KARAOKE DJ</h1>
  <p class="sub">Cette soirée demande un mot de passe — demandez au DJ.</p>
  <label>MOT DE PASSE</label>
  <input id="pw" type="password" autocomplete="off">
  <button id="go">ENTRER</button>
  <p id="gerr" class="hint"></p>
</div>
<script>
"use strict";
const $ = id => document.getElementById(id);
let PW = "";
try { PW = localStorage.getItem("kdj_pw") || ""; } catch (e) {}
function gate(show) { $("gate").hidden = !show; if (show) $("pw").focus(); }
async function ping() {
  try { gate(!(await fetch("/api/ping?pw=" + encodeURIComponent(PW))).ok); }
  catch (e) {}
}
ping();
$("go").addEventListener("click", async () => {
  PW = $("pw").value;
  try {
    if ((await fetch("/api/ping?pw=" + encodeURIComponent(PW))).ok) {
      try { localStorage.setItem("kdj_pw", PW); } catch (e) {}
      gate(false);
    } else {
      $("gerr").textContent = "Mauvais mot de passe — réessayez.";
    }
  } catch (e) { $("gerr").textContent = "Impossible de joindre le DJ."; }
});
$("pw").addEventListener("keydown", ev => {
  if (ev.key === "Enter") $("go").click();
});
try { $("name").value = localStorage.getItem("kdj_name") || ""; } catch (e) {}
$("name").addEventListener("input", () => {
  try { localStorage.setItem("kdj_name", $("name").value); } catch (e) {}
});
let timer = 0, toastTimer = 0;
function toast(msg, err) {
  const t = $("toast");
  t.textContent = msg;
  t.className = (err ? "err " : "") + "show";
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => { t.className = err ? "err" : ""; }, 2600);
}
function esc(s) {
  return s.replace(/[&<>"]/g, c =>
    ({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;"}[c]));
}
function fmt(ms) {
  const s = Math.round(ms / 1000);
  return Math.floor(s / 60) + ":" + String(s % 60).padStart(2, "0");
}
$("q").addEventListener("input", () => {
  clearTimeout(timer);
  timer = setTimeout(search, 250);
});
async function search() {
  const q = $("q").value.trim();
  if (q.length < 2) {
    $("list").innerHTML = '<p class="hint">Tapez au moins 2 lettres pour chercher.</p>';
    return;
  }
  try {
    const rs = await fetch("/api/search?q=" + encodeURIComponent(q) +
                           "&pw=" + encodeURIComponent(PW));
    if (rs.status === 401) { gate(true); return; }
    const rows = await rs.json();
    if (!rows.length) {
      $("list").innerHTML = '<p class="hint">Aucun résultat.</p>';
      return;
    }
    $("list").innerHTML = rows.map(r =>
      '<div class="row"><div class="meta"><div class="t">' + esc(r.t) +
      '</div><div class="a">' + esc(r.a || "—") +
      (r.d > 0 ? " · " + fmt(r.d) : "") +
      '</div></div><button data-id="' + r.id + '">DEMANDER</button></div>').join("");
  } catch (e) {
    $("list").innerHTML = '<p class="hint">Recherche impossible — le DJ est-il en ligne ?</p>';
  }
}
$("list").addEventListener("click", async ev => {
  const b = ev.target.closest("button");
  if (!b) return;
  const name = $("name").value.trim();
  if (!name) { toast("Entrez d'abord votre nom !", true); $("name").focus(); return; }
  b.disabled = true;
  try {
    const rs = await fetch("/api/request", {
      method: "POST",
      body: new URLSearchParams({ id: b.dataset.id, singer: name, pw: PW }),
    });
    if (rs.status === 401) { gate(true); b.disabled = false; return; }
    const j = await rs.json();
    toast(j.msg || (rs.ok ? "Demande envoyée !" : "Échec de la demande"), !rs.ok);
    if (!rs.ok) b.disabled = false;
  } catch (e) {
    toast("Impossible de joindre le DJ.", true);
    b.disabled = false;
  }
});
</script></body></html>)HTML";

// ------------------------------------------------------------------- helpers

static std::string jesc(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (const unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        default:
            if (c < 0x20) {
                char b[8];
                snprintf(b, 8, "\\u%04x", c);
                o += b;
            } else {
                o += char(c);
            }
        }
    }
    return o;
}

// First up private-network IPv4, preferring Wi-Fi/Ethernet over virtual
// adapters (VPNs, VM bridges) so the printed URL is the one phones can reach.
static std::string lanIp() {
    ULONG sz = 16 * 1024;
    std::vector<uint8_t> buf(sz);
    auto* aa = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    if (GetAdaptersAddresses(AF_INET,
                             GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST |
                                 GAA_FLAG_SKIP_DNS_SERVER,
                             nullptr, aa, &sz) != NO_ERROR)
        return "";
    std::string best;
    for (auto* ad = aa; ad; ad = ad->Next) {
        if (ad->OperStatus != IfOperStatusUp) continue;
        if (ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK || ad->IfType == IF_TYPE_TUNNEL)
            continue;
        for (auto* ua = ad->FirstUnicastAddress; ua; ua = ua->Next) {
            if (ua->Address.lpSockaddr->sa_family != AF_INET) continue;
            char ip[INET_ADDRSTRLEN]{};
            auto* sin = reinterpret_cast<sockaddr_in*>(ua->Address.lpSockaddr);
            inet_ntop(AF_INET, &sin->sin_addr, ip, sizeof(ip));
            const bool priv = !strncmp(ip, "192.168.", 8) || !strncmp(ip, "10.", 3) ||
                              !strncmp(ip, "172.", 4);
            const bool real = ad->IfType == IF_TYPE_IEEE80211 ||
                              ad->IfType == IF_TYPE_ETHERNET_CSMACD;
            if (priv && real) return ip;
            if (best.empty() && priv) best = ip;
        }
    }
    return best;
}

// -------------------------------------------------------------------- server

RequestServer::RequestServer() = default;
RequestServer::~RequestServer() { stop(); }

bool RequestServer::start(const std::wstring& dbPath, int firstPort) {
    stop();
    db_ = std::make_unique<Db>();
    if (!db_->open(dbPath)) {
        db_.reset();
        return false;
    }
    srv_ = std::make_unique<httplib::Server>();
    // One worker thread: handlers share db_ and run strictly one at a time.
    // ponytail: plenty for a barful of phones; bump if it ever queues.
    srv_->new_task_queue = [] { return new httplib::ThreadPool(1); };

    srv_->Get("/", [this](const httplib::Request&, httplib::Response& rs) {
        rs.set_content(lang_.load() ? kPageFr : kPage, "text/html; charset=utf-8");
    });

    const auto authed = [this](const httplib::Request& rq) {
        std::lock_guard<std::mutex> lk(mx_);
        return pass_.empty() || rq.get_param_value("pw") == pass_;
    };
    const auto deny = [](httplib::Response& rs) {
        rs.status = 401;
        rs.set_content("{\"msg\":\"password required\"}", "application/json");
    };

    srv_->Get("/api/ping", [authed, deny](const httplib::Request& rq,
                                          httplib::Response& rs) {
        if (!authed(rq)) return deny(rs);
        rs.set_content("{\"ok\":true}", "application/json");
    });

    // Phones only see singable material: CDG songs (mp3g / karaoke zips) plus
    // audio/video whose title or filename says "karaoke". Plain music videos
    // and regular audio are the DJ's, not the crowd's.
    static const char kKaraokeOnly[] =
        "IFNULL(hidden,0)=0 AND "
        "(type IN ('mp3g','karaoke_zip') OR (type IN ('audio','video') AND "
        "(fold(title) LIKE '%karaoke%' OR fold(path) LIKE '%karaoke%')))";

    srv_->Get("/api/search", [this, authed, deny](const httplib::Request& rq,
                                                  httplib::Response& rs) {
        if (!authed(rq)) return deny(rs);
        const std::wstring term = wide(rq.get_param_value("q"));
        std::string j = "[";
        if (term.size() >= 2) {
            const std::string sql =
                std::string("SELECT id,artist,title,duration_ms FROM media_item "
                            "WHERE (fold(title) LIKE ?1 OR fold(artist) LIKE ?1 "
                            "OR fold(path) LIKE ?1) AND ") +
                kKaraokeOnly +
                " ORDER BY artist COLLATE NOCASE, title COLLATE NOCASE LIMIT 30";
            Db::Stmt q;
            db_->prepare(q, sql.c_str());
            q.bind(1, "%" + utf8(foldW(term)) + "%");
            bool first = true;
            while (q.step()) {
                const std::wstring artist = wide(q.colText(1));
                const std::wstring title = wide(q.colText(2));
                char head[64];
                snprintf(head, 64, "%s{\"id\":%lld,\"d\":%lld,", first ? "" : ",",
                         static_cast<long long>(q.colInt(0)),
                         static_cast<long long>(q.colInt(3)));
                j += head;
                j += "\"t\":\"" + jesc(utf8(title)) + "\",";
                j += "\"a\":\"" + jesc(utf8(artist)) + "\"}";
                first = false;
            }
        }
        rs.set_content(j + "]", "application/json");
    });

    srv_->Post("/api/request", [this, authed, deny](const httplib::Request& rq,
                                                    httplib::Response& rs) {
        if (!authed(rq)) return deny(rs);
        const bool fr = lang_.load() == 1;
        const auto reply = [&](int code, const char* en, const char* frMsg) {
            rs.status = code;
            rs.set_content(std::string("{\"msg\":\"") + (fr ? frMsg : en) +
                               "\"}",
                           "application/json");
        };
        const int64_t id = atoll(rq.get_param_value("id").c_str());
        std::wstring singer = wide(rq.get_param_value("singer"));
        while (!singer.empty() && iswspace(singer.front())) singer.erase(0, 1);
        while (!singer.empty() && iswspace(singer.back())) singer.pop_back();
        if (singer.size() > 40) singer.resize(40);
        if (id <= 0 || singer.empty())
            return reply(400, "Missing name or song.",
                         "Nom ou chanson manquant.");

        // Per-phone throttle: one request each 10 s keeps double-taps and
        // pranksters out without any account machinery.
        static std::map<std::string, std::chrono::steady_clock::time_point> last;
        const auto now = std::chrono::steady_clock::now();
        auto it = last.find(rq.remote_addr);
        if (it != last.end() && now - it->second < std::chrono::seconds(10))
            return reply(429, "Easy! Wait a few seconds between requests.",
                         "Doucement ! Attendez quelques secondes.");

        Db::Stmt q; // same karaoke-only rule as search: a hand-crafted POST
                    // can't request a plain music video either
        const std::string sql =
            std::string("SELECT id,artist,title,path,type,duration_ms "
                        "FROM media_item WHERE id=?1 AND ") +
            kKaraokeOnly;
        db_->prepare(q, sql.c_str());
        q.bind(1, id);
        if (!q.step())
            return reply(404, "That song can't be requested.",
                         "Cette chanson ne peut pas être demandée.");
        Match m;
        m.id = q.colInt(0);
        m.artist = wide(q.colText(1));
        m.title = wide(q.colText(2));
        m.label = m.artist.empty() ? m.title : m.artist + L" - " + m.title;
        m.path = wide(q.colText(3));
        m.type = q.colText(4);
        m.durMs = q.colInt(5);

        {
            std::lock_guard<std::mutex> lk(mx_);
            for (const PhoneRequest& p : inbox_)
                if (p.song.id == id && p.singer == singer)
                    return reply(200, "Already requested — you're in!",
                                 "Déjà demandé — c'est noté !");
            if (inbox_.size() >= 100)
                return reply(503, "Request list is full.",
                             "La liste de demandes est pleine.");
            inbox_.push_back({singer, std::move(m)});
            pending_.store(true);
        }
        last[rq.remote_addr] = now;
        reply(200, "Request sent! Watch the rotation.",
              "Demande envoyée ! Surveillez la rotation.");
    });

    port_ = 0;
    for (int p = firstPort; p < firstPort + 10; ++p) {
        if (srv_->bind_to_port("0.0.0.0", p)) {
            port_ = p;
            break;
        }
    }
    if (!port_) {
        srv_.reset();
        db_.reset();
        return false;
    }
    th_ = std::thread([this]() { srv_->listen_after_bind(); });
    running_.store(true);
    return true;
}

void RequestServer::stop() {
    if (srv_) srv_->stop();
    if (th_.joinable()) th_.join();
    srv_.reset();
    db_.reset();
    running_.store(false);
    pending_.store(false);
    std::lock_guard<std::mutex> lk(mx_);
    inbox_.clear();
}

std::wstring RequestServer::url() const {
    if (!running_.load()) return L"";
    const unsigned long long now = GetTickCount64();
    if (urlAt_ && now - urlAt_ < 10000) return urlCache_;
    urlAt_ = now;
    const std::string ip = lanIp();
    urlCache_ = ip.empty() ? L""
                           : L"http://" + wide(ip) + L":" +
                                 std::to_wstring(port_) + L"/";
    return urlCache_;
}

void RequestServer::setPassword(const std::wstring& pass) {
    std::wstring t = pass;
    while (!t.empty() && iswspace(t.front())) t.erase(0, 1);
    while (!t.empty() && iswspace(t.back())) t.pop_back();
    std::lock_guard<std::mutex> lk(mx_);
    pass_ = utf8(t);
}

std::vector<PhoneRequest> RequestServer::take() {
    std::lock_guard<std::mutex> lk(mx_);
    pending_.store(false);
    std::vector<PhoneRequest> out = std::move(inbox_);
    inbox_.clear();
    return out;
}
