/* RVDP 1.0 browser codec and transactional framebuffer. No browser APIs needed. */
(function(root) {
  'use strict';
  const MAX=16417;
  function packet(type,seq,payload=new Uint8Array(),minor=0) {
    if(payload.length>16401) throw Error('Packet too large');
    const b=new Uint8Array(16+payload.length), d=new DataView(b.buffer);
    if(minor!==0&&minor!==1)throw Error('Unsupported version');
    b.set([82,86,68,80,1,minor,type,0]);d.setUint32(8,seq);d.setUint32(12,payload.length);b.set(payload,16);return b;
  }
  function hello(control,keyboard=false) {const b=new Uint8Array(13),d=new DataView(b.buffer);b.set([1,0,1,Number(keyboard)]);d.setUint32(4,1);d.setUint32(8,16384);b[12]=Number(control);return b;}
  class Display {
    constructor(){this.last=null;this.width=0;this.frame=null;this.lastFrame=null;this.needsFull=true;this.granted=false;this.capable=false;this.keyboard=false;this.minor=0;this.count=0;}
    resync(){this.frame=null;this.count=0;this.needsFull=true;}
    receive(buffer) {
      if(!(buffer instanceof ArrayBuffer)||buffer.byteLength<16||buffer.byteLength>MAX)throw Error('Invalid packet size');
      const d=new DataView(buffer),a=new Uint8Array(buffer);
      if(a[0]!==82||a[1]!==86||a[2]!==68||a[3]!==80||a[4]!==1||a[5]>1||a[7]||d.getUint32(12)!==a.length-16)throw Error('Invalid RVDP header');
      const seq=d.getUint32(8),type=a[6],n=a.length-16;
      if((this.last===null&&seq!==0)||(this.last!==null&&seq!==((this.last+1)>>>0)))throw Error('Stale sequence');this.last=seq;
      if(type===10){if(n<2||n>130)throw Error('Invalid device error');const error=Error(new TextDecoder().decode(a.subarray(18)));error.code=d.getUint16(16);throw error;}
      if(type===2){
        if(this.width||n!==14)throw Error('Invalid display info');
        const w=d.getUint16(16),h=d.getUint16(18),features=d.getUint32(26);
        if(!w||w>2048||!h||h>2048||w*h>1048576||a[20]!==1||a[21]||features>3||((features&2)&&a[5]!==1))throw Error('Unsupported display');
        this.minor=a[5];this.keyboard=Boolean(features&2);this.width=w;this.height=h;this.capable=Boolean(features&1);this.pixels=new Uint8ClampedArray(w*h*4);for(let i=3;i<this.pixels.length;i+=4)this.pixels[i]=255;
        return 'display';
      }
      if(!this.width)throw Error('Missing display info');
      if(a[5]!==this.minor)throw Error('Session version changed');
      if(type===11){if(n!==1||a[16]>1||(a[16]&&!this.capable))throw Error('Invalid grant');this.granted=Boolean(a[16]);return 'control';}
      if(type===3){
        if(n<17)throw Error('Truncated rectangle');
        const f=d.getUint32(16),x=d.getUint16(20),y=d.getUint16(22),w=d.getUint16(24),h=d.getUint16(26),length=d.getUint32(29);
        if(!w||!h||x+w>this.width||y+h>this.height||a[28]!==1||length!==w*h*2||length>16384||n!==17+length)throw Error('Invalid rectangle');
        if(this.lastFrame!==null&&(!((f-this.lastFrame)>>>0)||((f-this.lastFrame)>>>0)>=0x80000000))throw Error('Stale frame');
        // Keep the first tile of a replacement snapshot. Discarding it and
        // requesting another repaint can create an endless resync cycle.
        if(this.frame!==null&&this.frame!==f)this.resync();
        if(this.frame===null){this.frame=f;this.staged=this.pixels.slice();this.coverage=new Uint8Array(this.width*this.height);}
        if(++this.count>512)throw Error('Too many rectangles');
        for(let row=0;row<h;row++)for(let col=0;col<w;col++){
          const rgb=d.getUint16(33+(row*w+col)*2),at=(y+row)*this.width+x+col,i=at*4;
          this.staged[i]=Math.round((rgb>>>11)*255/31);this.staged[i+1]=Math.round(((rgb>>>5)&63)*255/63);this.staged[i+2]=Math.round((rgb&31)*255/31);this.coverage[at]=1;
        }return null;
      }
      if(type===4){
        if(n!==11||a[22]>1||!d.getUint16(20)||d.getUint16(20)>512)throw Error('Invalid frame end');
        const full=Boolean(a[22]);
        if(this.frame!==d.getUint32(16)||this.count!==d.getUint16(20)||(full&&!this.coverage.every(Boolean))||(this.needsFull&&!full)){this.resync();return 'resync';}
        this.pixels=this.staged;this.lastFrame=this.frame;this.frame=null;this.count=0;this.needsFull=false;return 'frame';
      }
      if((type===8||type===9)&&n===4)return type===8?'ping':'pong';
      if(type===10&&n>=2&&n<=130)throw Error('Device rejected the session');
      throw Error('Unsupported device packet');
    }
  }
  const api={packet,hello,Display};if(typeof module!=='undefined'&&module.exports)module.exports=api;else root.RVDP=api;
})(typeof globalThis!=='undefined'?globalThis:this);
