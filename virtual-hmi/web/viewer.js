/* Same protocol as iShareScreen; server owns all UI state and pixels. */
(() => {
  'use strict';
  const $=id=>document.getElementById(id),canvas=$('screen'),ctx=canvas.getContext('2d');
  let ws=null,display=null,sequence=0,pressed=false,retry=null,attempt=0,bytes=0,frames=0,lastRx=0,openedAt=0,watchdog=null,generation=0;
  const address=location.protocol==='https:'?location.host:`${location.hostname}:${location.port||81}`;
  const heldKeys=new Map(),special={Backspace:0xff08,Tab:0xff09,Enter:0xff0d,Escape:0xff1b,Home:0xff50,ArrowLeft:0xff51,ArrowRight:0xff53,PageUp:0xff55,PageDown:0xff56,End:0xff57,Delete:0xffff};
  $('endpoint').value=new URLSearchParams(location.search).get('ws')||`${location.protocol==='https:'?'wss':'ws'}://${address}/rvdp`;
  function send(type,payload) {if(ws?.readyState===WebSocket.OPEN){if(ws.bufferedAmount>4096){ws.close();return;}ws.send(RVDP.packet(type,sequence,payload,display?.minor||0));sequence=(sequence+1)>>>0;}}
  function key(code,down){const p=new Uint8Array(5);new DataView(p.buffer).setUint32(0,code);p[4]=Number(down);send(7,p);}
  function release(event=3){if(pressed&&display?.granted)pointer(0,0,false,event);pressed=false;if(display?.granted&&display.keyboard)for(const code of heldKeys.values())key(code,false);heldKeys.clear();}
  function pointer(x,y,down,event){const p=new Uint8Array(6),d=new DataView(p.buffer);d.setUint16(0,x);d.setUint16(2,y);p[4]=Number(down);p[5]=event;send(6,p);}
  function connect(){
    clearTimeout(retry);clearInterval(watchdog);generation++;const current=generation;
    if(ws){ws.onclose=null;ws.close();}release();
    sequence=0;display=new RVDP.Display();bytes=frames=0;
    try{const u=new URL($('endpoint').value);if(!['ws:','wss:'].includes(u.protocol)||u.username||u.password||u.search||u.hash||u.pathname!=='/rvdp')throw Error('Use a ws:// or wss:// /rvdp URL');ws=new WebSocket(u);}catch(e){$('status').textContent=e.message;return;}
    ws.binaryType='arraybuffer';$('status').textContent='Connecting…';
    ws.onopen=()=>{lastRx=openedAt=Date.now();send(1,RVDP.hello($('control').checked,true));watchdog=setInterval(()=>{if(Date.now()-lastRx>15000||(!frames&&Date.now()-openedAt>10000)){ws.close();return;}const p=new Uint8Array(4);new DataView(p.buffer).setUint32(0,Date.now()>>>0);send(8,p);},5000);};
    ws.onmessage=e=>{
      if(current!==generation)return;
      try{
        const event=display.receive(e.data);lastRx=Date.now();bytes+=e.data.byteLength;
        if(event==='display'){canvas.width=display.width;canvas.height=display.height;}
        if(event==='frame'){ctx.putImageData(new ImageData(display.pixels,display.width,display.height),0,0);frames++;attempt=0;$('status').textContent=display.granted?'Connected · pointer control granted'+(display.keyboard?' · keyboard ready':''):'Connected · view only';}
        if(event==='resync')send(5,new Uint8Array([1]));
        if(event==='ping')send(9,new Uint8Array(e.data,16));
        $('metrics').textContent=`${frames} complete batches · ${(bytes/1024).toFixed(1)} KiB received`;
      }catch(error){$('status').textContent=error.message;ws.close(1002);}
    };
    ws.onclose=()=>{if(current!==generation)return;clearInterval(watchdog);pressed=false;$('status').textContent='Disconnected · reconnecting…';retry=setTimeout(connect,Math.min(10000,500*2**Math.min(attempt++,5)));};
    ws.onerror=()=>{$('status').textContent='Connection failed';};
  }
  for(const name of ['pointerdown','pointermove','pointerup','pointercancel'])canvas.addEventListener(name,e=>{
    if(!display?.granted||!$('control').checked)return;e.preventDefault();const box=canvas.getBoundingClientRect();
    const x=Math.max(0,Math.min(canvas.width-1,Math.floor((e.clientX-box.left)*canvas.width/box.width))),y=Math.max(0,Math.min(canvas.height-1,Math.floor((e.clientY-box.top)*canvas.height/box.height)));
    if(name==='pointerdown'){pressed=true;canvas.focus();canvas.setPointerCapture(e.pointerId);}
    if(name==='pointermove'&&!pressed)return;
    if(name==='pointerup'||name==='pointercancel')pressed=false;
    pointer(x,y,pressed,{pointermove:0,pointerdown:1,pointerup:2,pointercancel:3}[name]);
  });
  canvas.addEventListener('keydown',e=>{if(!display?.granted||!display.keyboard||e.ctrlKey||e.metaKey||e.altKey)return;const code=special[e.key]||(e.key.length===1&&e.key.charCodeAt(0)>=32&&e.key.charCodeAt(0)<=126?e.key.charCodeAt(0):0);if(!code)return;e.preventDefault();heldKeys.set(e.code,code);key(code,true);});
  canvas.addEventListener('keyup',e=>{const code=heldKeys.get(e.code);if(code&&display?.granted&&display.keyboard){e.preventDefault();key(code,false);}heldKeys.delete(e.code);});
  canvas.addEventListener('blur',()=>release());
  window.addEventListener('blur',()=>release());document.addEventListener('visibilitychange',()=>{if(document.hidden)release();});
  $('connect').onclick=connect;$('control').onchange=()=>{release();connect();};$('refresh').onclick=()=>{display?.resync();send(5,new Uint8Array([2]));};connect();
})();
