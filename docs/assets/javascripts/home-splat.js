/*
 * Homepage hero canvas (overrides/home.html, #gsm-splat).
 *
 * Moved verbatim out of an inline <script> in overrides/home.html: templates
 * under overrides/ may not carry script (tests/ci/check_overrides_no_inline_script.py),
 * and browser-executed code lives here, under docs/assets/javascripts/ (R3).
 * Loaded on every page through extra_javascript in mkdocs.yml; it does nothing
 * on a page without the canvas. document$ re-runs it after Material instant
 * navigation, and the data-init flag keeps it to one init per canvas.
 */
// Signature splat viewport: a warm, depth-shaded point cloud of an interior
// capture — sorted back-to-front, near points larger/brighter. Not neon.
(function(){
  function initSplat(){
    var cv = document.getElementById('gsm-splat');
    if (!cv || cv.dataset.init) return;
    cv.dataset.init = '1';
    var dpr = Math.min(window.devicePixelRatio||1, 2);
    function size(){ var r=cv.getBoundingClientRect(); cv.width=Math.max(1,r.width*dpr); cv.height=Math.max(1,r.height*dpr); }
    size();
    var ctx = cv.getContext('2d');
    var seed=99; var rnd=function(){ return (seed=(seed*1664525+1013904223)%4294967296)/4294967296; };
    var pal=['#c9a87d','#b98a5c','#8f6b49','#d9c9a6','#e7d7b0','#6f8358','#5c6f45','#a9b8c8','#d8e2ee','#7a5f45','#3f342a'];
    var pts=[];
    function add(n, fx){ for(var i=0;i<n;i++){ pts.push(fx()); } }
    add(340, function(){ var u=rnd(), v=rnd(); var z=0.2+v*0.8; var x=(u-0.5)*1.9*z; var y=0.42+(1-z)*0.02; return {x:x, y:y, z:z, c:pal[[0,1,2,10][Math.floor(rnd()*4)]], s:1}; });
    add(240, function(){ var x=(rnd()-0.5)*1.5; var y=(rnd()*0.5)-0.1; return {x:x, y:y, z:0.95, c:pal[[3,4,7][Math.floor(rnd()*3)]], s:1}; });
    add(120, function(){ var x=-0.55+rnd()*0.28; var y=-0.12+rnd()*0.36; return {x:x, y:y, z:0.9, c:pal[8], s:1.1}; });
    add(260, function(){ var a=rnd()*6.28, r=rnd()*0.22; var x=0.12+Math.cos(a)*r; var y=0.16+Math.sin(a)*r*0.6; return {x:x, y:y, z:0.42+rnd()*0.12, c:pal[[1,2,9,7][Math.floor(rnd()*4)]], s:1}; });
    add(180, function(){ var a=rnd()*6.28, r=rnd()*0.16; var x=0.5+Math.cos(a)*r*0.6; var y=-0.02+Math.sin(a)*r-0.05; return {x:x, y:y, z:0.5+rnd()*0.1, c:pal[[5,6][Math.floor(rnd()*2)]], s:1}; });
    add(70, function(){ return {x:(rnd()-0.5)*1.8, y:(rnd()-0.5)*0.9, z:0.3+rnd()*0.6, c:pal[Math.floor(rnd()*pal.length)], s:.6}; });
    function hexToRgb(h){return [parseInt(h.slice(1,3),16),parseInt(h.slice(3,5),16),parseInt(h.slice(5,7),16)];}
    var rgb = pts.map(function(p){return hexToRgb(p.c);});
    function draw(){
      var W=cv.width, H=cv.height; ctx.clearRect(0,0,W,H);
      var order=pts.map(function(_,i){return i;}).sort(function(a,b){return pts[b].z-pts[a].z;});
      var cx=W*0.5, cy=H*0.46;
      for(var k=0;k<order.length;k++){
        var p=pts[order[k]];
        var persp = 0.62/(p.z*0.9+0.25);
        var px = cx + p.x*W*0.44*persp;
        var py = cy + p.y*H*0.9*persp;
        var depth = 1-(p.z-0.2)/0.85;
        var rad = (0.8 + depth*3.4)*p.s*dpr;
        var c=rgb[order[k]];
        var fog = 0.35 + depth*0.65;
        var rr=Math.round(c[0]*fog+10*(1-fog)), gg=Math.round(c[1]*fog+14*(1-fog)), bb=Math.round(c[2]*fog+20*(1-fog));
        ctx.globalAlpha = 0.5 + depth*0.45;
        ctx.fillStyle='rgb('+rr+','+gg+','+bb+')';
        ctx.beginPath(); ctx.arc(px,py,rad,0,7); ctx.fill();
      }
      ctx.globalAlpha=1;
      var bx=cx+0.12*W*0.44*(0.62/(0.48*0.9+0.25)), by=cy+0.16*H*0.9*(0.62/(0.48*0.9+0.25));
      ctx.strokeStyle='rgba(84,169,236,.9)'; ctx.lineWidth=1.2*dpr;
      var w=W*0.16, h=H*0.26; ctx.strokeRect(bx-w/2, by-h/2, w, h);
      ctx.fillStyle='rgba(84,169,236,.95)';
      [[bx-w/2,by-h/2],[bx+w/2,by-h/2],[bx-w/2,by+h/2],[bx+w/2,by+h/2]].forEach(function(cc){ctx.fillRect(cc[0]-2*dpr,cc[1]-2*dpr,4*dpr,4*dpr);});
    }
    draw();
    window.addEventListener('resize', function(){ size(); draw(); });
  }
  if (window.document$ && typeof window.document$.subscribe === 'function') {
    window.document$.subscribe(function(){ initSplat(); });
  } else if (document.readyState !== 'loading') {
    initSplat();
  } else {
    document.addEventListener('DOMContentLoaded', initSplat);
  }
})();
