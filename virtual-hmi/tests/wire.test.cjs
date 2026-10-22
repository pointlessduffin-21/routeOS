const {test}=require('node:test');
const assert=require('node:assert/strict');
const W=require('../web/wire.js');
const vectors=require('../../protocol/vectors/v1.json');
const data=name=>Uint8Array.from(Buffer.from(vectors[name],'hex')).buffer;
test('HELLO matches independent golden bytes',()=>{
  assert.equal(Buffer.from(W.packet(1,0,W.hello(false))).toString('hex'),vectors.hello_view);
});
test('RGB565 order and transactional frame presentation',()=>{
  const d=new W.Display();d.receive(data('display_rgb_row'));d.receive(data('rect_rgb_row'));
  assert.equal(d.pixels[0],0);assert.equal(d.receive(data('frame_end')),'frame');
  assert.deepEqual(Array.from(d.pixels),[255,0,0,255,0,255,0,255,0,0,255,255]);
});
for(const name of ['bad_magic','unsupported_version','truncated','oversized_length'])test(name,()=>{
  assert.throws(()=>new W.Display().receive(data(name)));
});
test('Incomplete full snapshots require resync',()=>{
  const d=new W.Display();const display=new Uint8Array(data('display_rgb_row'));new DataView(display.buffer).setUint16(18,2);
  d.receive(display.buffer);d.receive(data('rect_rgb_row'));
  assert.equal(d.receive(data('frame_end')),'resync');assert.equal(d.needsFull,true);
});
test('Stale sequence rejected',()=>{
  const d=new W.Display();d.receive(data('display_rgb_row'));assert.throws(()=>d.receive(data('display_rgb_row')));
});
test('Random malformed buffers fail cleanly',()=>{
  for(let n=0;n<256;n++){const b=new Uint8Array(n);b.fill(n);assert.throws(()=>new W.Display().receive(b.buffer));}
});
test('Keyboard 1.1 negotiation preserves legacy HELLO',()=>{
  const h=W.hello(true,true);assert.equal(h[3],1);
  const info=new Uint8Array(14),v=new DataView(info.buffer);v.setUint16(0,240);v.setUint16(2,135);info[4]=1;v.setUint32(10,3);
  const d=new W.Display();assert.equal(d.receive(W.packet(2,0,info,1).buffer),'display');assert.equal(d.keyboard,true);assert.equal(d.minor,1);
  assert.throws(()=>new W.Display().receive(W.packet(2,0,info,0).buffer));
});
