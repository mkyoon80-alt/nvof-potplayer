document.documentElement.classList.add('js');
const menu=document.getElementById('menu'),nav=document.getElementById('toc');
menu.addEventListener('click',()=>{const open=menu.getAttribute('aria-expanded')!=='true';menu.setAttribute('aria-expanded',String(open));nav.classList.toggle('open',open)});
nav.addEventListener('click',e=>{if(e.target.closest('a')){nav.classList.remove('open');menu.setAttribute('aria-expanded','false')}});
const rate=document.getElementById('source-rate');rate.addEventListener('change',()=>{document.getElementById('rate-result').textContent=rate.value+' fps'});
let folded=[];window.addEventListener('beforeprint',()=>{folded=[...document.querySelectorAll('details:not([open])')];folded.forEach(d=>d.open=true)});window.addEventListener('afterprint',()=>folded.forEach(d=>d.open=false));document.getElementById('print').addEventListener('click',()=>window.print());
if('IntersectionObserver' in window){
 const links=[...nav.querySelectorAll('a')],sections=[...document.querySelectorAll('main section')],visible=new Map();
 const observer=new IntersectionObserver(entries=>{
  entries.forEach(e=>visible.set(e.target.id,e.isIntersecting));
  const active=sections.find(s=>visible.get(s.id));
  if(active)links.forEach(a=>{if(a.hash==='#'+active.id)a.setAttribute('aria-current','location');else a.removeAttribute('aria-current')});
 },{rootMargin:'-90px 0px -50% 0px'});
 sections.forEach(s=>observer.observe(s));
}
