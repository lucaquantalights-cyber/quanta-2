// provision_page.h - first-time Wi-Fi setup page served from the controller's hotspot.
// Same flow as the reef doser's Provisioner: pick a network from a scan, save, then stay on the
// page while the controller connects, and show the home IP and http://quanta.local to write down.
#pragma once
#include <Arduino.h>

const char WIFI_SETUP_HTML[] PROGMEM = R"QPROV(<!doctype html><html lang="en"><head><meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1,viewport-fit=cover">
<meta name="theme-color" content="#081017"><title>Quanta Wi-Fi setup</title>
<style>
:root{--bg:#edf1f2;--card:#fbfcfc;--ink:#0f1d24;--muted:#5a7079;--line:#d2dcdf;--accent:#3f4bd1;--ok:#1d8457;--bad:#c23b3b;--sunk:#e3e9eb}
@media (prefers-color-scheme:dark){:root{--bg:#071017;--card:#0f1b23;--ink:#e2ebef;--muted:#8ea3ad;--line:#223440;--accent:#8f97ff;--ok:#45c48d;--bad:#f07a7a;--sunk:#16252f;color-scheme:dark}}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--ink);font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif;padding:24px 16px 40px}
.card{max-width:460px;margin:0 auto;background:var(--card);border:1px solid var(--line);border-radius:16px;padding:22px 18px;display:flex;flex-direction:column;gap:14px}
h1{font-size:22px;margin:0;letter-spacing:-.01em}
p{margin:0}.muted{color:var(--muted);font-size:14px}
label{display:flex;flex-direction:column;gap:6px;font-size:14px;color:var(--muted)}
select,input{font:inherit;color:var(--ink);background:var(--bg);border:1px solid var(--line);border-radius:10px;padding:11px 12px;width:100%}
.btn{font:inherit;font-weight:650;border:0;border-radius:12px;padding:13px 16px;background:var(--accent);color:#fff;cursor:pointer;width:100%;text-align:center;text-decoration:none;display:block}
.btn.ghost{background:transparent;color:var(--accent);border:1px solid var(--line);font-weight:500}
.row{display:flex;gap:8px;align-items:center}
.note{background:color-mix(in srgb,#f0a456 16%,var(--card));border:1px solid color-mix(in srgb,#f0a456 45%,transparent);border-radius:12px;padding:11px 12px;font-size:14px}
.url{font:600 19px/1.3 ui-monospace,Menlo,Consolas,monospace;color:var(--accent);word-break:break-all}
.spin{width:22px;height:22px;border-radius:50%;border:3px solid var(--sunk);border-top-color:var(--accent);animation:s .9s linear infinite;flex:none}
@keyframes s{to{transform:rotate(360deg)}}
.big{font-size:20px;font-weight:700}.ok{color:var(--ok)}.bad{color:var(--bad)}
[hidden]{display:none!important}
</style></head><body>
<div class="card" id="form">
  <h1>Connect your Quanta controller</h1>
  <p class="muted">Pick your home Wi-Fi so you can reach the controller from any phone in the house. The lights already run on their own.</p>
  <label>Network<select id="pick"><option value="">Looking for networks…</option></select></label>
  <label>Network name<input id="ssid" autocomplete="off" autocapitalize="none" placeholder="Pick above or type it"></label>
  <label>Password<input id="pass" type="password" autocomplete="off"></label>
  <label class="row" style="flex-direction:row;color:var(--muted)"><input type="checkbox" id="show" style="width:auto"> Show password</label>
  <button class="btn" id="save">Save and connect</button>
  <a class="btn ghost" href="/?app">Skip, use without home Wi-Fi</a>
</div>
<div class="card" id="status" hidden></div>
<script>
const $=id=>document.getElementById(id);
const esc=s=>String(s).replace(/[&<>"']/g,c=>({"&":"&amp;","<":"&lt;",">":"&gt;",'"':"&quot;","'":"&#39;"}[c]));
let ssidSaved="";
async function scan(){
  const pick=$("pick");
  try{
    const r=await fetch("/scan",{cache:"no-store"}); const nets=await r.json();
    pick.innerHTML=`<option value="">${nets.length?nets.length+" networks found":"No networks found, type it below"}</option>`;
    for(const n of nets){
      const o=document.createElement("option"); o.value=n.ssid;
      const bars=n.rssi>-60?"▂▄▆":n.rssi>-75?"▂▄ ":"▂  ";
      o.textContent=`${n.ssid}   ${bars}${n.open?"":"  🔒"}`; pick.appendChild(o);
    }
  }catch(e){ pick.innerHTML='<option value="">Couldn\'t scan, type it below</option>'; }
}
$("pick").onchange=e=>{ if(e.target.value){ $("ssid").value=e.target.value; $("pass").focus(); } };
$("show").onchange=e=>{ $("pass").type=e.target.checked?"text":"password"; };
$("save").onclick=async()=>{
  const ssid=$("ssid").value.trim(); if(!ssid){ $("ssid").focus(); return; }
  ssidSaved=ssid; $("save").disabled=true;
  try{
    await fetch("/save-wifi",{method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify({ssid,pass:$("pass").value})});
  }catch(e){}
  $("form").hidden=true; $("status").hidden=false; check();
};
async function check(){
  const s=$("status");
  try{
    const r=await fetch("/connect-status?ts="+Date.now(),{cache:"no-store"}); const j=await r.json();
    if(j.connected){
      s.innerHTML=`<p class="big ok">Connected!</p>
        <p><b>Write this down.</b> This is how you'll open the controller from now on:</p>
        <p class="url">http://${esc(j.ip)}</p><p class="muted">or</p><p class="url">http://${esc(j.mdns)}.local</p>
        <div class="note">The controller's setup hotspot turns off in about 2 minutes. Then switch your phone back to <b>${esc(ssidSaved)}</b> and open the address above.</div>
        <p class="muted">Some Android phones don't open <b>.local</b> addresses; use the numbers if it doesn't work.</p>`;
      return;
    }
    if(j.failed){
      s.innerHTML=`<p class="big bad">Couldn't connect to ${esc(ssidSaved)}</p>
        <p class="muted">Check the network name and password. The controller uses 2.4 GHz Wi-Fi, so a 5 GHz-only network won't show up or connect.</p>
        <button class="btn" onclick="location.reload()">Try again</button>`;
      return;
    }
    s.innerHTML=`<div class="row"><span class="spin"></span><span class="big">Connecting to ${esc(ssidSaved)}…</span></div>
      <p class="muted">${j.elapsed} seconds. Stay on this page.</p>
      <div class="note">If your phone says this network has <b>no internet</b>, choose <b>Stay connected</b> until the address appears.</div>`;
  }catch(e){}
  setTimeout(check,2000);
}
scan();
</script></body></html>)QPROV";
