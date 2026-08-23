#include "cauce/app/WebAssets.h"

namespace cauce::app {

const char kIndexHtml[] = R"rawliteral(<!DOCTYPE html>
<html lang="es">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>CAUCE</title>
<style>
:root{--bg:#0f1720;--card:#182430;--tx:#e8eef4;--mut:#8aa0b4;--ac:#39c2a7;--warn:#e6b455;--bad:#e26d5a}
*{box-sizing:border-box}
body{margin:0;background:var(--bg);color:var(--tx);font:16px/1.45 system-ui,-apple-system,"Segoe UI",Roboto,sans-serif}
header{display:flex;align-items:baseline;gap:.5rem;padding:.8rem 1rem;border-bottom:1px solid #223140;position:sticky;top:0;background:var(--bg)}
header h1{font-size:1.05rem;margin:0;letter-spacing:.12em;color:var(--ac)}
#nodeid{color:var(--mut);font-size:.9rem}
.pill{margin-left:auto;font-size:.78rem;padding:.15rem .6rem;border-radius:999px;border:1px solid var(--mut);color:var(--mut)}
.pill.ok{border-color:var(--ac);color:var(--ac)}
.pill.warn{border-color:var(--warn);color:var(--warn)}
.pill.bad{border-color:var(--bad);color:var(--bad)}
main{max-width:760px;margin:0 auto;padding:1rem}
.cards{display:grid;grid-template-columns:repeat(auto-fit,minmax(150px,1fr));gap:.7rem}
.card{background:var(--card);border-radius:14px;padding:.85rem 1rem}
.card .k{font-size:.75rem;text-transform:uppercase;letter-spacing:.08em;color:var(--mut)}
.card .v{font-size:1.9rem;font-weight:600;margin-top:.15rem}
.card .u{font-size:.95rem;color:var(--mut);margin-left:.15rem}
.card.q-SUSPECT .v{color:var(--warn)}.card.q-INVALID,.card.q-MISSING .v{color:var(--bad)}
section{margin-top:1.2rem}
.box{background:var(--card);border-radius:14px;padding:.85rem 1rem}
canvas{width:100%;height:auto;display:block}
nav{display:flex;gap:.4rem;margin-top:.5rem;flex-wrap:wrap}
button,a.btn{background:#223140;border:0;color:var(--tx);padding:.35rem .8rem;border-radius:8px;font-size:.85rem;cursor:pointer;text-decoration:none;display:inline-block}
button.on{background:var(--ac);color:#06251f;font-weight:600}
details{background:var(--card);border-radius:14px;padding:.6rem 1rem;margin-top:1.2rem}
summary{cursor:pointer;color:var(--mut)}
pre{overflow:auto;font-size:.78rem;color:var(--mut)}
form label{display:block;margin-top:.6rem;font-size:.85rem;color:var(--mut)}
input{width:100%;padding:.5rem;border-radius:8px;border:1px solid #31465a;background:#0f1720;color:var(--tx);font-size:1rem}
#cfgmsg{margin-left:.6rem;font-size:.85rem}
.err{color:var(--bad)} .okc{color:var(--ac)}
footer{text-align:center;color:var(--mut);font-size:.75rem;padding:1.2rem 0}
</style>
</head>
<body>
<header><h1>CAUCE</h1><span id="nodeid">&mdash;</span><span id="state" class="pill">conectando&hellip;</span></header>
<main>
<section class="cards" id="cards"></section>
<section class="box">
<canvas id="chart" width="720" height="240"></canvas>
<nav id="ranges">
<button data-h="1" data-i18n="r1h">1h</button><button data-h="6" class="on" data-i18n="r6h">6h</button><button data-h="24" data-i18n="r24h">24h</button><button data-h="168" data-i18n="r7d">7d</button>
<span style="flex:1"></span>
<button id="sT" class="on" data-i18n="seriesT">T&deg;</button><button id="sH" data-i18n="seriesH">HR%</button>
</nav>
<div id="chartmsg" style="color:var(--mut);font-size:.8rem"></div>
</section>
<section class="box" style="display:flex;gap:.6rem;flex-wrap:wrap">
<a class="btn" id="dlcsv" data-i18n="exportCsv">Exportar CSV</a><a class="btn" id="dljson" data-i18n="exportJson">Exportar JSON</a>
<a class="btn" href="/api/v1/health" target="_blank">/health</a>
</section>
<details><summary data-i18n="healthTitle">Salud del sistema</summary><pre id="health">-</pre></details>
<details><summary data-i18n="configTitle">Configuraci&oacute;n (admin)</summary>
<form id="cfg">
<label data-i18n="cfgInterval">Intervalo de medici&oacute;n (segundos)</label>
<input name="sampling_interval_s" type="number" min="10" max="3600">
<label data-i18n="cfgToken">Token de administraci&oacute;n</label>
<input name="__token" type="password" autocomplete="off">
<label>&nbsp;</label>
<button type="submit" data-i18n="cfgApply">Aplicar</button><span id="cfgmsg"></span>
</form>
</details>
<footer id="lastupd"><span data-i18n="footer">CAUCE &middot; monitoreo comunitario microclim&aacute;tico</span> <button id="langEs" style="margin-left:.6rem">ES</button><button id="langEn">EN</button></footer>
</main>
<script>
"use strict";
const $=id=>document.getElementById(id);
const I18N={
 es:{r1h:"1h",r6h:"6h",r24h:"24h",r7d:"7d",seriesT:"T\u00b0",seriesH:"HR%",
     exportCsv:"Exportar CSV",exportJson:"Exportar JSON",
     healthTitle:"Salud del sistema",configTitle:"Configuraci\u00f3n (admin)",
     cfgInterval:"Intervalo de medici\u00f3n (segundos)",cfgToken:"Token de administraci\u00f3n",
     cfgApply:"Aplicar",footer:"CAUCE \u00b7 monitoreo comunitario microclim\u00e1tico",
     connecting:"conectando\u2026",nodata:"sin datos",
     cardQuality:"calidad",cardLatest:"\u00daltima medici\u00f3n",cardTimeNode:"Tiempo del nodo",
     timeSynced:"sincronizado",timeUncertain:"incierto",noMeasurements:"sin mediciones",
     lblTemp:"Temperatura",lblHum:"Humedad",lblPress:"Presi\u00f3n",lblLight:"Luz",lblBatt:"Bater\u00eda",
     chartLoading:"cargando\u2026",chartNotEnough:"sin suficientes datos en el per\u00edodo",
     chartNow:"ahora",chartTemp:"Temperatura",chartHum:"Humedad relativa",chartSamples:"muestras",
     cfgNeedToken:"ingres\u00e1 el token",cfgNothing:"nada que aplicar",cfgApplied:"\u2713 aplicado",
     cfgNetError:"error de red"},
 en:{r1h:"1h",r6h:"6h",r24h:"24h",r7d:"7d",seriesT:"T\u00b0",seriesH:"RH%",
     exportCsv:"Export CSV",exportJson:"Export JSON",
     healthTitle:"System health",configTitle:"Configuration (admin)",
     cfgInterval:"Measurement interval (seconds)",cfgToken:"Admin token",
     cfgApply:"Apply",footer:"CAUCE \u00b7 community microclimate monitoring",
     connecting:"connecting\u2026",nodata:"no data",
     cardQuality:"quality",cardLatest:"Last measurement",cardTimeNode:"Node clock",
     timeSynced:"synced",timeUncertain:"uncertain",noMeasurements:"no measurements",
     lblTemp:"Temperature",lblHum:"Humidity",lblPress:"Pressure",lblLight:"Light",lblBatt:"Battery",
     chartLoading:"loading\u2026",chartNotEnough:"not enough data in this period",
     chartNow:"now",chartTemp:"Temperature",chartHum:"Relative humidity",chartSamples:"samples",
     cfgNeedToken:"enter the token",cfgNothing:"nothing to apply",cfgApplied:"\u2713 applied",
     cfgNetError:"network error"}};
let LANG=localStorage.getItem("cauce-lang")||
  ((navigator.language||"es").toLowerCase().startsWith("en")?"en":"es");
function tr(k){return (I18N[LANG]&&I18N[LANG][k])||(I18N.es&&I18N.es[k])||k}
function applyLang(){
 document.querySelectorAll("[data-i18n]").forEach(el=>{el.innerHTML=tr(el.dataset.i18n)});
 $("langEs").classList.toggle("on",LANG==="es");
 $("langEn").classList.toggle("on",LANG==="en");
 localStorage.setItem("cauce-lang",LANG);
}
let VAR_LABEL={};
function rebuildLabels(){VAR_LABEL={air_temperature:tr("lblTemp"),relative_humidity:tr("lblHum"),pressure:tr("lblPress"),illuminance:tr("lblLight"),battery_voltage:tr("lblBatt")}}
const VAR_UNIT={air_temperature:"\u00b0C",relative_humidity:"%",pressure:"hPa",illuminance:"lx",battery_voltage:"V"};
let hours=6, series="T";
function fmtAgo(ts){if(!ts)return"-";const s=Math.max(0,(Date.now()-ts)/1000);if(s<90)return"hace "+Math.round(s)+" s";if(s<5400)return"hace "+Math.round(s/60)+" min";if(s<172800)return"hace "+Math.round(s/3600)+" h";return"hace "+Math.round(s/86400)+" d"}
function qClass(q){return q==="SUSPECT"?"q-SUSPECT":(q==="INVALID"||q==="MISSING")?"q-INVALID":""}
function card(k,v,u,q){return '<div class="card '+(q?("q-"+q):"")+'"><div class="k">'+k+'</div><div class="v">'+v+'<span class="u">'+u+'</span></div>'+(q?'<div class="k">calidad '+q+'</div>':"")+'</div>'}
async function j(url){const r=await fetch(url,{cache:"no-store"});if(!r.ok)throw new Error(r.status);return r.json()}
async function refreshStatus(){
 try{
  const [node,status]=await Promise.all([j("/api/v1/node"),j("/api/v1/status")]);
  $("nodeid").textContent=node.node_id+(node.site_id?" \u00b7 "+node.site_id:"");
  const st=$("state"),net=status.net_state;
  st.textContent=net;
  st.className="pill "+(net==="CONNECTED"?"ok":(net==="OFFLINE"||net==="AP_MODE"||net==="AP_FALLBACK")?"warn":"");
  const c=$("cards");c.innerHTML="";
  const L=status.latest;
  if(L&&L.value!==null){
    c.innerHTML+=card(VAR_LABEL[L.variable]||L.variable,L.value.toFixed(1),VAR_UNIT[L.variable]||"",L.quality);
    c.innerHTML+=card(tr("cardQuality"),L.quality,"","");
    c.innerHTML+=card(tr("cardLatest"),fmtAgo(L.timestamp_utc_ms),"","");
    c.innerHTML+=card(tr("cardTimeNode"),status.time_valid?tr("timeSynced"):tr("timeUncertain"),"","");
  } else { c.innerHTML=card(tr("cardQuality"),tr("noMeasurements"),"","MISSING"); }
 }catch(e){$("state").textContent=tr("nodata");$("state").className="pill bad"}
}
function drawChart(pts){
 const cv=$("chart"),ctx=cv.getContext("2d");
 ctx.clearRect(0,0,cv.width,cv.height);
 if(pts.length<2){$("chartmsg").textContent=tr("chartNotEnough");return}
 const xs=pts.map(p=>p.t),ys=pts.map(p=>p.v);
 let y0=Math.min.apply(null,ys),y1=Math.max.apply(null,ys);
 if(y1-y0<0.5){const m=(y1+y0)/2;y0=m-0.5;y1=m+0.5}
 const pad=34,W=cv.width-pad-8,H=cv.height-24,X=i=>pad+(W-pad-8)*i/(pts.length-1),Y=v=>8+H*(1-(v-y0)/(y1-y0));
 ctx.strokeStyle="#31465a";ctx.beginPath();ctx.moveTo(pad,8);ctx.lineTo(pad,8+H);ctx.lineTo(W,8+H);ctx.stroke();
 ctx.fillStyle="#8aa0b4";ctx.font="11px system-ui";
 ctx.fillText(y1.toFixed(1),2,Y(y1)+4);ctx.fillText(y0.toFixed(1),2,Y(y0)+4);
 const t0=new Date(xs[0]),t1=new Date(xs[xs.length-1]);
 ctx.fillText(t0.toISOString().slice(5,16).replace("T"," "),pad,cv.height-6);
 const lbl=t1.toISOString().slice(5,16).replace("T"," ");
 ctx.fillText(lbl,W-ctx.measureText(lbl).width,cv.height-6);
 ctx.strokeStyle="#39c2a7";ctx.lineWidth=1.6;ctx.beginPath();
 pts.forEach((p,i)=>{i?ctx.lineTo(X(i),Y(p.v)):ctx.moveTo(X(i),Y(p.v))});
 ctx.stroke();
 const last=pts[pts.length-1];
 $("chartmsg").textContent=(series==="T"?tr("chartTemp"):tr("chartHum"))+" \u00b7 "+pts.length+" "+tr("chartSamples")+" \u00b7 "+tr("chartNow")+" "+last.v.toFixed(1)+(series==="T"?" \u00b0C":" %");
}
async function loadChart(){
 $("chartmsg").textContent=tr("chartLoading");
 const from=new Date(Date.now()-hours*3600000).toISOString();
 try{
  const data=await j("/api/v1/measurements?from="+encodeURIComponent(from));
  const want=series==="T"?"air_temperature":"relative_humidity";
  drawChart(data.filter(m=>m.variable===want&&m.quality!=="MISSING").map(m=>({t:m.timestamp_utc_ms,v:m.value})));
 }catch(e){$("chartmsg").textContent="error cargando serie"}
}
document.getElementById("ranges").addEventListener("click",e=>{
 if(e.target.dataset.h){hours=+e.target.dataset.h;
  document.querySelectorAll("#ranges button[data-h]").forEach(b=>b.classList.toggle("on",b===e.target));
  loadChart()}
});
$("sT").onclick=()=>{series="T";$("sT").classList.add("on");$("sH").classList.remove("on");loadChart()};
$("sH").onclick=()=>{series="H";$("sH").classList.add("on");$("sT").classList.remove("on");loadChart()};
function setLinks(){const from=new Date(Date.now()-24*3600000).toISOString();const q="?from="+encodeURIComponent(from);
 $("dlcsv").href="/api/v1/export?format=csv"+q;$("dljson").href="/api/v1/export?format=json"+q}
async function refreshHealth(){
 try{const h=await j("/api/v1/health");
  $("health").textContent=JSON.stringify(h,null,1);
 }catch(e){}
}
$("cfg").addEventListener("submit",async ev=>{
 ev.preventDefault();
 const f=ev.target,msg=$("cfgmsg"),token=f.__token.value.trim();
 if(!token){msg.textContent=tr("cfgNeedToken");msg.className="err";return}
 const lines=[];
 const iv=f.sampling_interval_s.value.trim();
 if(iv)lines.push("sampling_interval_s="+iv);
 if(!lines.length){msg.textContent=tr("cfgNothing");return}
 try{
  const r=await fetch("/api/v1/config",{method:"POST",
   headers:{"Authorization":"Bearer "+token},
   body:lines.join("\n")+"\n"});
  const b=await r.json();
  if(r.ok){msg.textContent=tr("cfgApplied");msg.className="okc";refreshStatus()}
  else{msg.textContent=(b.errors||[b.error||("HTTP "+r.status)]).join("; ");msg.className="err"}
 }catch(e){msg.textContent=tr("cfgNetError");msg.className="err"}
});
async function tickAll(){await refreshStatus();setLinks();await refreshHealth()}
$("langEs").onclick=()=>{LANG="es";rebuildLabels();applyLang();refreshStatus().then(loadChart)};
$("langEn").onclick=()=>{LANG="en";rebuildLabels();applyLang();refreshStatus().then(loadChart)};
rebuildLabels();
applyLang();
tickAll().then(loadChart);
setInterval(refreshStatus,30000);
setInterval(refreshHealth,60000);
setInterval(()=>{loadChart()},120000);
</script>
</body>
</html>)rawliteral";

const size_t kIndexHtmlLen = sizeof(kIndexHtml) - 1;

}  // namespace cauce::app
