/* brdom.js - DOM simulado para o BROWSER.EXE rodar os scripts de uma pagina
 * no JS.EXE (o SpiderMonkey do port do Firefox, so com o interpretador).
 *
 *   (na pasta de trabalho)  JS.EXE --no-jit-backend D:\BRDOM.JS
 *
 * Le na pasta atual (pelo load() do shell: o readFile chama fstat, que falha
 * no SODOS, e o load tolera):
 *   DADOS.JS   var __BRDADOS = { url: "...", w: 1024, h: 768, html: "...",
 *                                scripts: { "src ou url": "S0.JS", ... } };
 *   S0.JS ...  os <script src> que o navegador ja baixou
 * Escreve na saida:
 *   [js] ...         mensagens (erros, console) para a barra de estado
 *   [js-alert] ...   texto de alert()
 *   [js-ir] url      a pagina mandou ir para url (location = ...)
 *   [js-falta] url   script que a pagina pediu e que nao foi baixado
 *   <<<BRDOM>>>
 *   o HTML depois dos scripts, sem <script>, <noscript> e comentarios
 *
 * E um "pre-render": roda os scripts na ordem, o DOMContentLoaded, o load e
 * os timers (ate VIRT_MAX ms de relogio simulado) e devolve a foto do DOM.
 * Depois disso a pagina volta a ser HTML: onclick e afins nao rodam mais.
 * Sem rede: fetch e XMLHttpRequest falham (o endereco vai para o log). */
(function (G) {
  "use strict";
  var print_ = G.print, putstr_ = G.putstr, evaluate_ = G.evaluate, timeout_ = G.timeout;
  var drain_ = typeof G.drainJobQueue === "function" ? G.drainJobQueue : function () {};
  var load_ = G.load;
  var dateNow_ = Date.now;
  var T0 = dateNow_();
  var BUDGET_MS = 8000;  /* tempo real maximo para scripts e timers */
  var VIRT_MAX = 4000;   /* relogio simulado: timers ate aqui */
  var TIMERS_MAX = 400;
  var HARD_S = 20;       /* corte do shell (timeout) para laco infinito */

  /* ---------- log ---------- */
  var LOG = [], LOG_MAX = 40, IR = null, FALTA = [];
  var DEBUG = !!G.__BRDEBUG; /* var __BRDEBUG = 1 antes: marcas de tempo na hora */
  function log(s) {
    if (LOG.length < LOG_MAX) LOG.push("[js] " + String(s).replace(/\s+/g, " ").slice(0, 240));
  }
  function mark(what) {
    if (DEBUG) putstr_("[t] " + (dateNow_() - T0) + " ms " + what + "\n");
  }
  function report(e, where) {
    var m = e && e.message ? e.message : String(e);
    var ln = e && e.lineNumber ? ":" + e.lineNumber : "";
    log("erro em " + short(where) + ln + ": " + m);
  }
  function short(u) {
    u = String(u || "");
    var i = u.lastIndexOf("/");
    return i >= 0 && u.length - i > 1 ? u.slice(i + 1, i + 60) : u.slice(0, 60);
  }
  function drain() {
    try { drain_(); } catch (e) { report(e, "promise"); }
  }

  function mkset(s) {
    var o = Object.create(null);
    s.split(" ").forEach(function (k) { o[k] = true; });
    return o;
  }

  /* ---------- entidades ---------- */
  var ENT = Object.create(null);
  ("nbsp iexcl cent pound curren yen brvbar sect uml copy ordf laquo not shy reg macr " +
   "deg plusmn sup2 sup3 acute micro para middot cedil sup1 ordm raquo frac14 frac12 " +
   "frac34 iquest Agrave Aacute Acirc Atilde Auml Aring AElig Ccedil Egrave Eacute Ecirc " +
   "Euml Igrave Iacute Icirc Iuml ETH Ntilde Ograve Oacute Ocirc Otilde Ouml times Oslash " +
   "Ugrave Uacute Ucirc Uuml Yacute THORN szlig agrave aacute acirc atilde auml aring " +
   "aelig ccedil egrave eacute ecirc euml igrave iacute icirc iuml eth ntilde ograve " +
   "oacute ocirc otilde ouml divide oslash ugrave uacute ucirc uuml yacute thorn yuml")
    .split(" ").forEach(function (n, i) { ENT[n] = String.fromCharCode(160 + i); });
  var MORE = { amp: "&", lt: "<", gt: ">", quot: '"', apos: "'", hellip: "\u2026",
    mdash: "\u2014", ndash: "\u2013", lsquo: "\u2018", rsquo: "\u2019", ldquo: "\u201C",
    rdquo: "\u201D", sbquo: "\u201A", bdquo: "\u201E", bull: "\u2022", trade: "\u2122",
    euro: "\u20AC", larr: "\u2190", uarr: "\u2191", rarr: "\u2192", darr: "\u2193",
    harr: "\u2194", thinsp: "\u2009", ensp: "\u2002", emsp: "\u2003", zwj: "\u200D",
    zwnj: "\u200C", lrm: "\u200E", rlm: "\u200F", OElig: "\u0152", oelig: "\u0153",
    Scaron: "\u0160", scaron: "\u0161", Yuml: "\u0178", fnof: "\u0192", circ: "\u02C6",
    tilde: "\u02DC", dagger: "\u2020", Dagger: "\u2021", permil: "\u2030",
    lsaquo: "\u2039", rsaquo: "\u203A", prime: "\u2032", Prime: "\u2033",
    minus: "\u2212", infin: "\u221E", ne: "\u2260", le: "\u2264", ge: "\u2265",
    hearts: "\u2665", check: "\u2713", star: "\u2606", starf: "\u2605" };
  Object.keys(MORE).forEach(function (k) { ENT[k] = MORE[k]; });
  var ENT_RE = /&(#[0-9]+|#[xX][0-9a-fA-F]+|[a-zA-Z][a-zA-Z0-9]*);?/g;
  function decode(s) {
    if (s.indexOf("&") < 0) return s;
    return s.replace(ENT_RE, function (m, e) {
      if (e.charAt(0) === "#") {
        var n = e.charAt(1) === "x" || e.charAt(1) === "X" ? parseInt(e.slice(2), 16) : parseInt(e.slice(1), 10);
        if (!(n > 0) || n > 0x10FFFF || (n >= 0xD800 && n <= 0xDFFF)) return "\uFFFD";
        return String.fromCodePoint(n);
      }
      var v = ENT[e];
      return v === undefined ? m : v;
    });
  }
  /* so cria string nova quando precisa: menos lixo para o GC no SODOS */
  var NEED_TEXT = /[&<>]/, NEED_ATTR = /[&"]/;
  function escText(s) {
    if (!NEED_TEXT.test(s)) return s;
    return s.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;");
  }
  function escAttr(s) {
    if (!NEED_ATTR.test(s)) return s;
    return s.replace(/&/g, "&amp;").replace(/"/g, "&quot;");
  }

  /* ---------- enderecos ---------- */
  var URL_RE = /^([a-zA-Z][a-zA-Z0-9+.\-]*):(\/\/(?:([^:@\/?#]*)(?::([^@\/?#]*))?@)?([^\/?#:]*|\[[^\]]*\])(?::(\d*))?)?([^?#]*)(\?[^#]*)?(#[\s\S]*)?$/;
  var DEF_PORT = { "http:": "80", "https:": "443", "ws:": "80", "wss:": "443", "ftp:": "21" };
  function parseURL(s) {
    var m = URL_RE.exec(String(s).trim());
    if (!m) return null;
    var u = {
      protocol: m[1].toLowerCase() + ":", auth: !!m[2], username: m[3] || "", password: m[4] || "",
      hostname: (m[5] || "").toLowerCase(), port: m[6] || "", pathname: m[7] || "",
      search: m[8] && m[8] !== "?" ? m[8] : "", hash: m[9] && m[9] !== "#" ? m[9] : ""
    };
    if (DEF_PORT[u.protocol] === u.port) u.port = "";
    if (u.auth && !u.pathname) u.pathname = "/";
    u.pathname = dots(u.pathname);
    return u;
  }
  function dots(p) {
    if (p.indexOf(".") < 0) return p;
    var out = [], parts = p.split("/");
    for (var i = 0; i < parts.length; i++) {
      var s = parts[i];
      if (s === "..") { if (out.length > 1) out.pop(); if (i === parts.length - 1) out.push(""); }
      else if (s === ".") { if (i === parts.length - 1) out.push(""); }
      else out.push(s);
    }
    return out.join("/");
  }
  function host(u) { return u.hostname + (u.port ? ":" + u.port : ""); }
  function origin(u) { return u.auth ? u.protocol + "//" + host(u) : "null"; }
  function fmtURL(u) {
    var a = u.username ? u.username + (u.password ? ":" + u.password : "") + "@" : "";
    return u.protocol + (u.auth ? "//" + a + host(u) : "") + u.pathname + u.search + u.hash;
  }
  function resolve(rel, base) {
    rel = String(rel == null ? "" : rel).replace(/^[\s\u0000-\u0020]+|[\s\u0000-\u0020]+$/g, "");
    if (/^[a-zA-Z][a-zA-Z0-9+.\-]*:/.test(rel)) { var a = parseURL(rel); return a ? fmtURL(a) : rel; }
    var b = parseURL(base);
    if (!b) return rel;
    var pre = b.protocol + (b.auth ? "//" + host(b) : "");
    if (rel.slice(0, 2) === "//") { var c = parseURL(b.protocol + rel); return c ? fmtURL(c) : rel; }
    if (rel === "") return pre + b.pathname + b.search;
    if (rel.charAt(0) === "#") return pre + b.pathname + b.search + rel;
    if (rel.charAt(0) === "?") return pre + b.pathname + rel;
    var path;
    if (rel.charAt(0) === "/") path = rel;
    else path = b.pathname.slice(0, b.pathname.lastIndexOf("/") + 1) + rel;
    var d = parseURL(pre + (path.charAt(0) === "/" ? "" : "/") + path);
    return d ? fmtURL(d) : pre + path;
  }

  /* ---------- nos ---------- */
  var VOID = mkset("area base br col embed hr img input link meta param source track wbr keygen");
  var RAWT = mkset("script style xmp iframe noembed noframes noscript");
  var RCDATA = mkset("textarea title");
  var CLOSE_P = mkset("address article aside blockquote center details dialog dir div dl " +
    "fieldset figcaption figure footer form h1 h2 h3 h4 h5 h6 header hgroup hr main menu " +
    "nav ol p pre section table ul li dd dt");
  var HEAD_OK = mkset("base link meta script style title noscript template");
  var SCOPE = mkset("table td th caption html body template object marquee applet button");
  var HEADING = mkset("h1 h2 h3 h4 h5 h6");
  var EMPTY_ARR = Object.freeze([]);

  function isEl(n) { return n.nodeType === 1; }

  class EventTarget_ {
    addEventListener(type, fn, opt) { addEL(this, type, fn, opt); }
    removeEventListener(type, fn) { removeEL(this, type, fn); }
    dispatchEvent(ev) { return dispatch(this, ev); }
  }

  class Node extends EventTarget_ {
    get parentNode() { return this._p; }
    get parentElement() { var p = this._p; return p && p.nodeType === 1 ? p : null; }
    get childNodes() { return this._c; }
    get firstChild() { return this._c[0] || null; }
    get lastChild() { var c = this._c; return c[c.length - 1] || null; }
    get nextSibling() {
      var p = this._p; if (!p) return null;
      var c = p._c; return c[c.indexOf(this) + 1] || null;
    }
    get previousSibling() {
      var p = this._p; if (!p) return null;
      var c = p._c, i = c.indexOf(this); return i > 0 ? c[i - 1] : null;
    }
    get ownerDocument() { return this.nodeType === 9 ? null : this._doc; }
    get isConnected() { var n = this; while (n._p) n = n._p; return n.nodeType === 9; }
    get nodeValue() { return this.nodeType === 3 || this.nodeType === 8 ? this.data : null; }
    set nodeValue(v) { if (this.nodeType === 3 || this.nodeType === 8) this.data = String(v); }
    get baseURI() { return DOC ? DOC.URL : ""; }
    getRootNode() { var n = this; while (n._p) n = n._p; return n; }
    hasChildNodes() { return this._c.length > 0; }
    appendChild(n) { return this.insertBefore(n, null); }
    insertBefore(n, ref) {
      if (n == null) throw new TypeError("insertBefore: no nulo");
      if (n.nodeType === 11) {
        var kids = n._c.slice();
        for (var i = 0; i < kids.length; i++) this.insertBefore(kids[i], ref);
        return n;
      }
      if (n._p) n._p._detach(n);
      var c = this._c, k = ref ? c.indexOf(ref) : -1;
      if (k < 0) c.push(n); else c.splice(k, 0, n);
      n._p = this;
      if (!PARSING) inserted(n);
      return n;
    }
    _detach(n) { var i = this._c.indexOf(n); if (i >= 0) this._c.splice(i, 1); n._p = null; }
    removeChild(n) { if (n && n._p === this) this._detach(n); return n; }
    replaceChild(n, old) { this.insertBefore(n, old); this._detach(old); return old; }
    remove() { if (this._p) this._p._detach(this); }
    contains(n) { while (n) { if (n === this) return true; n = n._p; } return false; }
    get textContent() {
      if (this.nodeType === 3 || this.nodeType === 8) return this.data;
      if (this.nodeType === 9) return null;
      var out = []; textOf(this, out); return out.join("");
    }
    set textContent(v) {
      if (this.nodeType === 3 || this.nodeType === 8) { this.data = String(v); return; }
      this._clear();
      if (v != null && v !== "") this.appendChild(this._doc.createTextNode(String(v)));
    }
    _clear() { var c = this._c; for (var i = 0; i < c.length; i++) c[i]._p = null; this._c = []; }
    cloneNode(deep) {
      var n;
      if (this.nodeType === 3) n = this._doc.createTextNode(this.data);
      else if (this.nodeType === 8) n = this._doc.createComment(this.data);
      else if (this.nodeType === 11) n = this._doc.createDocumentFragment();
      else if (this.nodeType === 1) {
        n = this._doc.createElement(this._n);
        for (var i = 0; i < this._ao.length; i++) n.setAttribute(this._ao[i], this._a[this._ao[i]]);
      } else return null;
      if (deep) for (var j = 0; j < this._c.length; j++) n.appendChild(this._c[j].cloneNode(true));
      return n;
    }
    _nodes(args) {
      var out = [];
      for (var i = 0; i < args.length; i++) {
        var a = args[i];
        out.push(a instanceof Node ? a : this._doc.createTextNode(String(a)));
      }
      return out;
    }
    append() { var ns = this._nodes(arguments); for (var i = 0; i < ns.length; i++) this.appendChild(ns[i]); }
    prepend() { var ns = this._nodes(arguments), f = this.firstChild; for (var i = 0; i < ns.length; i++) this.insertBefore(ns[i], f); }
    before() { var p = this._p; if (!p) return; var ns = this._nodes(arguments); for (var i = 0; i < ns.length; i++) p.insertBefore(ns[i], this); }
    after() {
      var p = this._p; if (!p) return;
      var ns = this._nodes(arguments), nx = this.nextSibling;
      for (var i = 0; i < ns.length; i++) p.insertBefore(ns[i], nx);
    }
    replaceWith() { var p = this._p; if (!p) return; this.before.apply(this, arguments); p._detach(this); }
    replaceChildren() { this._clear(); this.append.apply(this, arguments); }
    normalize() {}
    isEqualNode(o) { return o === this; }
    isSameNode(o) { return o === this; }
    compareDocumentPosition(o) {
      if (o === this) return 0;
      if (this.contains(o)) return 20;
      if (o.contains && o.contains(this)) return 10;
      var all = []; walk(this.getRootNode(), function (n) { all.push(n); });
      return all.indexOf(o) > all.indexOf(this) ? 4 : 2;
    }
    querySelector(s) { return qs(this, s, true); }
    querySelectorAll(s) { return qs(this, s, false); }
    getElementsByTagName(t) {
      t = String(t).toLowerCase();
      var out = []; walkEl(this, function (e) { if (t === "*" || e._n === t) out.push(e); }); return out;
    }
    getElementsByClassName(c) {
      var want = String(c).trim().split(/\s+/);
      var out = [];
      walkEl(this, function (e) { if (hasClasses(e, want)) out.push(e); });
      return out;
    }
    get children() { return this._c.filter(isEl); }
    get childElementCount() { return this.children.length; }
    get firstElementChild() { var c = this._c; for (var i = 0; i < c.length; i++) if (c[i].nodeType === 1) return c[i]; return null; }
    get lastElementChild() { var c = this._c; for (var i = c.length - 1; i >= 0; i--) if (c[i].nodeType === 1) return c[i]; return null; }
  }
  Node.ELEMENT_NODE = 1; Node.ATTRIBUTE_NODE = 2; Node.TEXT_NODE = 3; Node.COMMENT_NODE = 8;
  Node.DOCUMENT_NODE = 9; Node.DOCUMENT_TYPE_NODE = 10; Node.DOCUMENT_FRAGMENT_NODE = 11;
  Node.DOCUMENT_POSITION_PRECEDING = 2; Node.DOCUMENT_POSITION_FOLLOWING = 4;
  Node.DOCUMENT_POSITION_CONTAINS = 8; Node.DOCUMENT_POSITION_CONTAINED_BY = 16;
  Object.assign(Node.prototype, { ELEMENT_NODE: 1, TEXT_NODE: 3, COMMENT_NODE: 8, DOCUMENT_NODE: 9,
    DOCUMENT_FRAGMENT_NODE: 11 });

  function textOf(n, out) {
    var c = n._c;
    for (var i = 0; i < c.length; i++) {
      var k = c[i];
      if (k.nodeType === 3) out.push(k.data);
      else if (k.nodeType === 1) textOf(k, out);
    }
  }
  function walk(n, fn) {
    fn(n);
    var c = n._c;
    for (var i = 0; i < c.length; i++) walk(c[i], fn);
  }
  /* elementos abaixo de root (sem ele), em ordem; fn devolve true para parar */
  function walkEl(root, fn) {
    var stack = [root], idx = [0];
    while (stack.length) {
      var top = stack.length - 1, n = stack[top], i = idx[top];
      if (i >= n._c.length) { stack.pop(); idx.pop(); continue; }
      idx[top] = i + 1;
      var k = n._c[i];
      if (k.nodeType !== 1) continue;
      if (fn(k)) return k;
      if (k._c.length) { stack.push(k); idx.push(0); }
    }
    return null;
  }

  class CharacterData extends Node {
    get length() { return this.data.length; }
    appendData(s) { this.data += s; }
    substringData(o, n) { return this.data.substr(o, n); }
  }
  class Text extends CharacterData {
    get nodeName() { return "#text"; }
    get wholeText() { return this.data; }
    splitText(o) {
      var t = this._doc.createTextNode(this.data.slice(o));
      this.data = this.data.slice(0, o);
      if (this._p) this._p.insertBefore(t, this.nextSibling);
      return t;
    }
  }
  class Comment extends CharacterData {
    get nodeName() { return "#comment"; }
  }
  class DocumentFragment extends Node {
    get nodeName() { return "#document-fragment"; }
    getElementById(id) { return walkEl(this, function (e) { return e._a.id === id; }); }
  }

  function initNode(n, type, doc) {
    n.nodeType = type; n._p = null; n._c = type === 3 || type === 8 ? EMPTY_ARR : []; n._doc = doc;
  }

  /* ---------- elementos ---------- */
  class Element extends Node {
    get nodeName() { return this._n.toUpperCase(); }
    get tagName() { return this._n.toUpperCase(); }
    get localName() { return this._n; }
    get namespaceURI() { return this._ns || "http://www.w3.org/1999/xhtml"; }
    getAttribute(n) { var v = this._a[String(n).toLowerCase()]; return v === undefined ? null : v; }
    getAttributeNS(ns, n) { return this.getAttribute(n); }
    setAttribute(n, v) {
      n = String(n).toLowerCase();
      if (!(n in this._a)) this._ao.push(n);
      this._a[n] = String(v);
    }
    setAttributeNS(ns, n, v) { this.setAttribute(n, v); }
    removeAttribute(n) {
      n = String(n).toLowerCase();
      if (n in this._a) { delete this._a[n]; this._ao.splice(this._ao.indexOf(n), 1); }
    }
    removeAttributeNS(ns, n) { this.removeAttribute(n); }
    hasAttribute(n) { return String(n).toLowerCase() in this._a; }
    hasAttributes() { return this._ao.length > 0; }
    getAttributeNames() { return this._ao.slice(); }
    toggleAttribute(n, force) {
      var has = this.hasAttribute(n);
      if (force === undefined ? has : !force) { this.removeAttribute(n); return false; }
      if (!has) this.setAttribute(n, "");
      return true;
    }
    get attributes() {
      var self = this;
      var list = this._ao.map(function (n) { return { name: n, localName: n, value: self._a[n], nodeName: n, nodeValue: self._a[n], specified: true }; });
      list.getNamedItem = function (n) { n = String(n).toLowerCase(); for (var i = 0; i < list.length; i++) if (list[i].name === n) return list[i]; return null; };
      return list;
    }
    get className() { var v = this._a["class"]; return v === undefined ? "" : v; }
    set className(v) { this.setAttribute("class", v); }
    get classList() { return new ClassList(this); }
    get innerHTML() { var out = []; serChildren(this, out, false); return out.join(""); }
    set innerHTML(v) {
      this._clear();
      parseHTML(this._doc, String(v == null ? "" : v), this, true);
    }
    get outerHTML() { var out = []; ser(this, out, false); return out.join(""); }
    set outerHTML(v) {
      var p = this._p; if (!p) return;
      var f = fragment(this._doc, String(v), p);
      p.insertBefore(f, this); p._detach(this);
    }
    insertAdjacentHTML(pos, html) {
      pos = String(pos).toLowerCase();
      var ctx = pos === "beforebegin" || pos === "afterend" ? this._p : this;
      if (!ctx) return;
      this._adj(pos, fragment(this._doc, String(html), ctx));
    }
    insertAdjacentElement(pos, el) { this._adj(String(pos).toLowerCase(), el); return el; }
    insertAdjacentText(pos, t) { this._adj(String(pos).toLowerCase(), this._doc.createTextNode(String(t))); }
    _adj(pos, n) {
      if (pos === "beforebegin") { if (this._p) this._p.insertBefore(n, this); }
      else if (pos === "afterbegin") this.insertBefore(n, this.firstChild);
      else if (pos === "beforeend") this.appendChild(n);
      else if (pos === "afterend") { if (this._p) this._p.insertBefore(n, this.nextSibling); }
    }
    matches(s) { return matchesSel(this, s); }
    webkitMatchesSelector(s) { return matchesSel(this, s); }
    msMatchesSelector(s) { return matchesSel(this, s); }
    closest(s) { var n = this; while (n && n.nodeType === 1) { if (matchesSel(n, s)) return n; n = n._p; } return null; }
    get nextElementSibling() { var n = this.nextSibling; while (n && n.nodeType !== 1) n = n.nextSibling; return n; }
    get previousElementSibling() { var n = this.previousSibling; while (n && n.nodeType !== 1) n = n.previousSibling; return n; }
    get style() { return this._st || (this._st = makeStyle(this)); }
    set style(v) { this.setAttribute("style", v); }
    get dataset() { return this._ds || (this._ds = makeDataset(this)); }
    get innerText() { return this.textContent; }
    set innerText(v) { this.textContent = v; }
    get outerText() { return this.textContent; }
    getBoundingClientRect() { return rect0(); }
    getClientRects() { return []; }
    get offsetWidth() { return 0; } get offsetHeight() { return 0; }
    get offsetTop() { return 0; } get offsetLeft() { return 0; } get offsetParent() { return null; }
    get clientWidth() { return 0; } get clientHeight() { return 0; }
    get clientTop() { return 0; } get clientLeft() { return 0; }
    get scrollWidth() { return 0; } get scrollHeight() { return 0; }
    get scrollTop() { return 0; } set scrollTop(v) {}
    get scrollLeft() { return 0; } set scrollLeft(v) {}
    scrollIntoView() {} scrollTo() {} scrollBy() {} scroll() {}
    focus() { if (DOC) DOC._active = this; } blur() {}
    click() { dispatch(this, new MouseEvent("click", { bubbles: true, cancelable: true })); }
    attachShadow() {
      var f = this._doc.createDocumentFragment();
      f.host = this; f.mode = "open"; f.innerHTML = "";
      Object.defineProperty(f, "innerHTML", {
        get: function () { var o = []; serChildren(f, o, false); return o.join(""); },
        set: function (v) { f._clear(); parseHTML(f._doc, String(v), f, true); }
      });
      this._shadow = f;
      return f;
    }
    get shadowRoot() { return this._shadow || null; }
    animate() { return { finished: Promise.resolve(), cancel: function () {}, play: function () {}, pause: function () {}, onfinish: null }; }
    getAnimations() { return []; }
    requestFullscreen() { return Promise.reject(new Error("sem tela cheia")); }
  }
  class HTMLElement extends Element {}
  function rect0() { return { x: 0, y: 0, top: 0, left: 0, right: 0, bottom: 0, width: 0, height: 0, toJSON: function () { return this; } }; }

  /* propriedades que refletem atributos */
  function reflect(C, prop, attr, kind) {
    Object.defineProperty(C.prototype, prop, {
      configurable: true,
      get: function () {
        var v = this.getAttribute(attr);
        if (kind === "bool") return v !== null;
        if (kind === "num") { var n = parseInt(v, 10); return isNaN(n) ? (attr === "tabindex" ? -1 : 0) : n; }
        if (kind === "url") return v === null ? "" : resolve(v, DOC ? DOC._base : "");
        return v === null ? "" : v;
      },
      set: function (v) {
        if (kind === "bool") { if (v) this.setAttribute(attr, ""); else this.removeAttribute(attr); }
        else this.setAttribute(attr, v);
      }
    });
  }
  ("id title lang dir accessKey:accesskey alt type name rel target method placeholder " +
   "content charset media label autocomplete download crossOrigin:crossorigin integrity " +
   "loading srcset sizes poster preload enctype htmlFor:for referrerPolicy:referrerpolicy " +
   "inputMode:inputmode enterKeyHint:enterkeyhint pattern min max step accept nonce slot " +
   "role align valign bgColor:bgcolor color face border shape coords scope headers abbr " +
   "useMap:usemap dateTime:datetime cite kind srclang wrap").split(" ").forEach(function (p) {
    var k = p.split(":"); reflect(HTMLElement, k[0], (k[1] || k[0]).toLowerCase(), "str");
  });
  ("hidden disabled checked:checked selected readOnly:readonly required multiple async defer " +
   "autofocus autoplay controls loop muted noValidate:novalidate open reversed isMap:ismap " +
   "noModule:nomodule allowFullscreen:allowfullscreen inert draggable spellcheck").split(" ").forEach(function (p) {
    var k = p.split(":"); reflect(HTMLElement, k[0], (k[1] || k[0]).toLowerCase(), "bool");
  });
  ("tabIndex:tabindex width height colSpan:colspan rowSpan:rowspan cols rows size " +
   "maxLength:maxlength minLength:minlength start span").split(" ").forEach(function (p) {
    var k = p.split(":"); reflect(HTMLElement, k[0], (k[1] || k[0]).toLowerCase(), "num");
  });
  ("href src action formAction:formaction data longDesc:longdesc").split(" ").forEach(function (p) {
    var k = p.split(":"); reflect(HTMLElement, k[0], (k[1] || k[0]).toLowerCase(), "url");
  });
  Object.defineProperty(HTMLElement.prototype, "value", {
    configurable: true,
    get: function () {
      if (this._n === "textarea") return this._val !== undefined ? this._val : this.textContent;
      if (this._n === "select") {
        var o = this.querySelector("option[selected]") || this.querySelector("option");
        return o ? o.value : "";
      }
      var v = this.getAttribute("value");
      if (v === null && this._n === "option") return this.textContent.trim();
      return v === null ? "" : v;
    },
    set: function (v) {
      v = v == null ? "" : String(v);
      if (this._n === "textarea") { this._val = v; this.textContent = v; return; }
      if (this._n === "select") {
        var opts = this.getElementsByTagName("option");
        for (var i = 0; i < opts.length; i++) {
          if (opts[i].value === v) opts[i].setAttribute("selected", ""); else opts[i].removeAttribute("selected");
        }
        return;
      }
      this.setAttribute("value", v);
    }
  });
  Object.defineProperty(HTMLElement.prototype, "selectedIndex", {
    configurable: true,
    get: function () {
      var o = this.getElementsByTagName("option");
      for (var i = 0; i < o.length; i++) if (o[i].hasAttribute("selected")) return i;
      return o.length ? 0 : -1;
    },
    set: function (k) {
      var o = this.getElementsByTagName("option");
      for (var i = 0; i < o.length; i++) if (i === k) o[i].setAttribute("selected", ""); else o[i].removeAttribute("selected");
    }
  });
  Object.defineProperty(HTMLElement.prototype, "options", {
    configurable: true, get: function () { return this.getElementsByTagName("option"); }
  });
  Object.defineProperty(HTMLElement.prototype, "text", {
    configurable: true,
    get: function () { return this.textContent; },
    set: function (v) { this.textContent = v; }
  });
  Object.defineProperty(HTMLElement.prototype, "defaultValue", {
    configurable: true,
    get: function () { return this.getAttribute("value") || ""; },
    set: function (v) { this.setAttribute("value", v); }
  });
  Object.defineProperty(HTMLElement.prototype, "form", {
    configurable: true, get: function () { return this.closest("form"); }
  });
  Object.defineProperty(HTMLElement.prototype, "elements", {
    configurable: true, get: function () { return this.querySelectorAll("input,select,textarea,button"); }
  });
  Object.defineProperty(HTMLElement.prototype, "content", {
    configurable: true,
    get: function () {
      if (this._n !== "template") return this.getAttribute("content") || "";
      var f = this._doc.createDocumentFragment();
      for (var i = 0; i < this._c.length; i++) f.appendChild(this._c[i].cloneNode(true));
      return f;
    }
  });
  /* partes do endereco em <a> e <area> */
  ["protocol", "host", "hostname", "port", "pathname", "search", "hash", "origin", "username", "password"].forEach(function (k) {
    Object.defineProperty(HTMLElement.prototype, k, {
      configurable: true,
      get: function () {
        var u = parseURL(this.href || "");
        if (!u) return "";
        if (k === "host") return host(u);
        if (k === "origin") return origin(u);
        return u[k];
      },
      set: function (v) {
        var u = parseURL(this.href || ""); if (!u) return;
        if (k === "host") { var p = String(v).split(":"); u.hostname = p[0]; u.port = p[1] || ""; }
        else if (k === "search") u.search = v && String(v).charAt(0) !== "?" ? "?" + v : String(v);
        else if (k === "hash") u.hash = v && String(v).charAt(0) !== "#" ? "#" + v : String(v);
        else u[k] = String(v);
        this.setAttribute("href", fmtURL(u));
      }
    });
  });
  HTMLElement.prototype.submit = function () { log("form.submit() ignorado"); };
  HTMLElement.prototype.reset = function () {};
  HTMLElement.prototype.checkValidity = function () { return true; };
  HTMLElement.prototype.reportValidity = function () { return true; };
  HTMLElement.prototype.setCustomValidity = function () {};
  HTMLElement.prototype.select = function () {};
  HTMLElement.prototype.setSelectionRange = function () {};
  HTMLElement.prototype.play = function () { return Promise.reject(new Error("sem midia")); };
  HTMLElement.prototype.pause = function () {};
  HTMLElement.prototype.load = function () {};
  HTMLElement.prototype.canPlayType = function () { return ""; };
  HTMLElement.prototype.getContext = function () { return null; };
  HTMLElement.prototype.toDataURL = function () { return "data:,"; };
  HTMLElement.prototype.showModal = function () { this.setAttribute("open", ""); };
  HTMLElement.prototype.show = function () { this.setAttribute("open", ""); };
  HTMLElement.prototype.close = function () { this.removeAttribute("open"); };
  Object.defineProperty(HTMLElement.prototype, "complete", { configurable: true, get: function () { return true; } });
  Object.defineProperty(HTMLElement.prototype, "naturalWidth", { configurable: true, get: function () { return 0; } });
  Object.defineProperty(HTMLElement.prototype, "naturalHeight", { configurable: true, get: function () { return 0; } });
  Object.defineProperty(HTMLElement.prototype, "isContentEditable", { configurable: true, get: function () { return false; } });

  var TAGCLASS = {
    a: "Anchor", area: "Area", audio: "Audio", br: "BR", base: "Base", body: "Body",
    button: "Button", canvas: "Canvas", dl: "DList", data: "Data", datalist: "DataList",
    details: "Details", dialog: "Dialog", div: "Div", embed: "Embed", fieldset: "FieldSet",
    form: "Form", h1: "Heading", h2: "Heading", h3: "Heading", h4: "Heading", h5: "Heading",
    h6: "Heading", head: "Head", hr: "HR", html: "Html", iframe: "IFrame", img: "Image",
    input: "Input", label: "Label", legend: "Legend", li: "LI", link: "Link", map: "Map",
    menu: "Menu", meta: "Meta", meter: "Meter", object: "Object", ol: "OList",
    optgroup: "OptGroup", option: "Option", output: "Output", p: "Paragraph", param: "Param",
    picture: "Picture", pre: "Pre", progress: "Progress", q: "Quote", blockquote: "Quote",
    script: "Script", select: "Select", slot: "Slot", source: "Source", span: "Span",
    style: "Style", table: "Table", tbody: "TableSection", thead: "TableSection",
    tfoot: "TableSection", td: "TableCell", th: "TableCell", tr: "TableRow",
    template: "Template", textarea: "TextArea", time: "Time", title: "Title",
    track: "Track", ul: "UList", video: "Video", caption: "TableCaption",
    col: "TableCol", colgroup: "TableCol"
  };
  var CLASSES = Object.create(null);
  var MEDIA = class HTMLMediaElement extends HTMLElement {};
  Object.keys(TAGCLASS).forEach(function (t) {
    var name = "HTML" + TAGCLASS[t] + "Element";
    if (!CLASSES[name]) {
      var base = t === "audio" || t === "video" ? MEDIA : HTMLElement;
      CLASSES[name] = ({ [name]: class extends base {} })[name];
    }
  });
  CLASSES.HTMLMediaElement = MEDIA;
  CLASSES.HTMLUnknownElement = class HTMLUnknownElement extends HTMLElement {};
  class SVGElement extends Element {}

  /* ---------- classList, style, dataset ---------- */
  class ClassList {
    constructor(el) { this._el = el; }
    _get() { var v = this._el._a["class"]; return v ? v.trim().split(/\s+/) : []; }
    _set(l) { this._el.setAttribute("class", l.join(" ")); }
    get length() { return this._get().length; }
    get value() { return this._el.className; }
    item(i) { return this._get()[i] || null; }
    contains(c) { return this._get().indexOf(String(c)) >= 0; }
    add() { var l = this._get(); for (var i = 0; i < arguments.length; i++) if (l.indexOf(String(arguments[i])) < 0) l.push(String(arguments[i])); this._set(l); }
    remove() { var r = Array.prototype.map.call(arguments, String); this._set(this._get().filter(function (c) { return r.indexOf(c) < 0; })); }
    toggle(c, force) {
      var has = this.contains(c);
      if (force === undefined ? has : !force) { this.remove(c); return false; }
      if (!has) this.add(c);
      return true;
    }
    replace(a, b) { var l = this._get(), i = l.indexOf(String(a)); if (i < 0) return false; l[i] = String(b); this._set(l); return true; }
    forEach(fn, t) { this._get().forEach(fn, t); }
    toString() { return this._el.className; }
    [Symbol.iterator]() { return this._get()[Symbol.iterator](); }
  }

  function parseStyle(s) {
    var m = Object.create(null), order = [];
    String(s || "").split(";").forEach(function (d) {
      var i = d.indexOf(":");
      if (i < 0) return;
      var k = d.slice(0, i).trim().toLowerCase(), v = d.slice(i + 1).trim();
      if (!k) return;
      if (!(k in m)) order.push(k);
      m[k] = v;
    });
    return { m: m, order: order };
  }
  function kebab(p) {
    if (p === "cssFloat") return "float";
    if (p.slice(0, 2) === "--") return p;
    return p.replace(/[A-Z]/g, function (c) { return "-" + c.toLowerCase(); }).replace(/^(webkit|moz|ms|o)-/, "-$1-");
  }
  function setProp(el, k, v) {
    var st = parseStyle(el.getAttribute("style"));
    v = v == null ? "" : String(v).replace(/\s*!important\s*$/, "");
    if (v === "") { if (k in st.m) { delete st.m[k]; st.order.splice(st.order.indexOf(k), 1); } }
    else { if (!(k in st.m)) st.order.push(k); st.m[k] = v; }
    var txt = st.order.map(function (x) { return x + ": " + st.m[x]; }).join("; ");
    if (txt) el.setAttribute("style", txt); else el.removeAttribute("style");
  }
  function makeStyle(el) {
    return new Proxy({}, {
      get: function (t, p) {
        if (typeof p !== "string") return undefined;
        if (p === "cssText") return el.getAttribute("style") || "";
        if (p === "setProperty") return function (k, v) { setProp(el, String(k).toLowerCase(), v); };
        if (p === "getPropertyValue") return function (k) { return parseStyle(el.getAttribute("style")).m[String(k).toLowerCase()] || ""; };
        if (p === "removeProperty") return function (k) { k = String(k).toLowerCase(); var o = parseStyle(el.getAttribute("style")).m[k] || ""; setProp(el, k, ""); return o; };
        if (p === "getPropertyPriority") return function () { return ""; };
        if (p === "item") return function (i) { return parseStyle(el.getAttribute("style")).order[i] || ""; };
        if (p === "length") return parseStyle(el.getAttribute("style")).order.length;
        if (p === "parentRule") return null;
        if (p === "toString") return function () { return "[object CSSStyleDeclaration]"; };
        return parseStyle(el.getAttribute("style")).m[kebab(p)] || "";
      },
      set: function (t, p, v) {
        if (typeof p !== "string") return true;
        if (p === "cssText") el.setAttribute("style", v); else setProp(el, kebab(p), v);
        return true;
      },
      has: function (t, p) { return typeof p === "string"; }
    });
  }
  function makeDataset(el) {
    function attr(p) { return "data-" + String(p).replace(/[A-Z]/g, function (c) { return "-" + c.toLowerCase(); }); }
    return new Proxy({}, {
      get: function (t, p) { if (typeof p !== "string") return undefined; var v = el.getAttribute(attr(p)); return v === null ? undefined : v; },
      set: function (t, p, v) { if (typeof p === "string") el.setAttribute(attr(p), v); return true; },
      has: function (t, p) { return typeof p === "string" && el.hasAttribute(attr(p)); },
      deleteProperty: function (t, p) { el.removeAttribute(attr(p)); return true; },
      ownKeys: function () {
        return el._ao.filter(function (n) { return n.slice(0, 5) === "data-"; }).map(function (n) {
          return n.slice(5).replace(/-([a-z])/g, function (m, c) { return c.toUpperCase(); });
        });
      },
      getOwnPropertyDescriptor: function (t, p) {
        var v = el.getAttribute(attr(p));
        return v === null ? undefined : { value: v, writable: true, enumerable: true, configurable: true };
      }
    });
  }

  /* ---------- seletores ---------- */
  var SELCACHE = Object.create(null);
  function selError(s) { var e = new SyntaxError("'" + s + "' is not a valid selector"); e.name = "SyntaxError"; return e; }
  function parseSel(src) {
    var hit = SELCACHE[src];
    if (hit) return hit;
    var s = String(src), i = 0, n = s.length, groups = [], group = [], comb = null;
    function ws() { var had = false; while (i < n && /\s/.test(s.charAt(i))) { i++; had = true; } return had; }
    function ident() {
      var out = "";
      while (i < n) {
        var ch = s.charAt(i);
        if (ch === "\\") {
          var hx = /^[0-9a-fA-F]{1,6} ?/.exec(s.slice(i + 1));
          if (hx) { out += String.fromCodePoint(parseInt(hx[0], 16)); i += 1 + hx[0].length; }
          else { out += s.charAt(i + 1); i += 2; }
        } else if (/[\w\-]/.test(ch) || ch.charCodeAt(0) > 127) { out += ch; i++; }
        else break;
      }
      return out;
    }
    function compound() {
      var c = { tag: null, id: null, cls: null, attrs: null, ps: null };
      var any = false;
      if (s.charAt(i) === "*") { i++; any = true; }
      else if (/[\w\-\\]/.test(s.charAt(i)) || s.charCodeAt(i) > 127) { c.tag = ident().toLowerCase(); any = true; }
      for (;;) {
        var ch = s.charAt(i);
        if (ch === "#") { i++; c.id = ident(); any = true; }
        else if (ch === ".") { i++; (c.cls || (c.cls = [])).push(ident()); any = true; }
        else if (ch === "[") {
          i++; ws();
          var name = ident().toLowerCase(); ws();
          var op = null, val = null, flag = "";
          var m = /^([~|^$*]?=)/.exec(s.slice(i));
          if (m) {
            op = m[1]; i += op.length; ws();
            var q = s.charAt(i);
            if (q === '"' || q === "'") {
              var e = s.indexOf(q, i + 1);
              if (e < 0) throw selError(src);
              val = s.slice(i + 1, e).replace(/\\(.)/g, "$1"); i = e + 1;
            } else val = ident();
            ws();
            if (/[iIsS]/.test(s.charAt(i))) {
              var j2 = i + 1;
              while (/\s/.test(s.charAt(j2))) j2++;
              if (s.charAt(j2) === "]") { flag = s.charAt(i).toLowerCase(); i = j2; }
            }
          }
          if (s.charAt(i) !== "]") throw selError(src);
          i++;
          (c.attrs || (c.attrs = [])).push({ name: name, op: op, val: val, ci: flag === "i" });
          any = true;
        } else if (ch === ":") {
          i++;
          var pe = false;
          if (s.charAt(i) === ":") { i++; pe = true; }
          var pn = ident().toLowerCase(), arg = null;
          if (s.charAt(i) === "(") {
            var depth = 1, st = ++i;
            while (i < n && depth) { if (s.charAt(i) === "(") depth++; else if (s.charAt(i) === ")") depth--; i++; }
            arg = s.slice(st, i - 1).trim();
          }
          (c.ps || (c.ps = [])).push({ name: pe ? "::" + pn : pn, arg: arg });
          any = true;
        } else break;
      }
      if (!any) throw selError(src);
      return c;
    }
    ws();
    while (i < n) {
      group.push({ c: comb, s: compound() });
      var had = ws();
      var ch = s.charAt(i);
      if (ch === ",") { i++; ws(); groups.push(group); group = []; comb = null; continue; }
      if (ch === ">" || ch === "+" || ch === "~") { comb = ch; i++; ws(); continue; }
      if (i >= n) break;
      if (had) { comb = " "; continue; }
      throw selError(src);
    }
    if (!group.length) throw selError(src);
    groups.push(group);
    SELCACHE[src] = groups;
    return groups;
  }
  function hasClasses(el, want) {
    var v = el._a["class"];
    if (!v) return false;
    var have = " " + v.replace(/\s+/g, " ") + " ";
    for (var i = 0; i < want.length; i++) if (have.indexOf(" " + want[i] + " ") < 0) return false;
    return true;
  }
  function prevEl(el) { var p = el._p; if (!p) return null; var c = p._c; for (var i = c.indexOf(el) - 1; i >= 0; i--) if (c[i].nodeType === 1) return c[i]; return null; }
  function elIndex(el, ofType, fromEnd) {
    var p = el._p; if (!p) return 1;
    var c = p._c, k = 0;
    if (!fromEnd) { for (var i = 0; i < c.length; i++) { var x = c[i]; if (x.nodeType === 1 && (!ofType || x._n === el._n)) { k++; if (x === el) return k; } } }
    else { for (var j = c.length - 1; j >= 0; j--) { var y = c[j]; if (y.nodeType === 1 && (!ofType || y._n === el._n)) { k++; if (y === el) return k; } } }
    return k;
  }
  function nth(arg, k) {
    arg = String(arg || "").replace(/\s+/g, "").toLowerCase();
    arg = arg.replace(/\bof.*$/, "");
    if (arg === "odd") return k % 2 === 1;
    if (arg === "even") return k % 2 === 0;
    var m = /^([+-]?\d*)n([+-]\d+)?$/.exec(arg);
    if (!m) return k === parseInt(arg, 10);
    var a = m[1] === "" || m[1] === "+" ? 1 : m[1] === "-" ? -1 : parseInt(m[1], 10);
    var b = m[2] ? parseInt(m[2], 10) : 0;
    if (a === 0) return k === b;
    var d = (k - b) / a;
    return d >= 0 && Math.floor(d) === d;
  }
  function matchPseudo(el, p) {
    switch (p.name) {
      case "first-child": return !prevEl(el);
      case "last-child": return !el.nextElementSibling;
      case "only-child": return !prevEl(el) && !el.nextElementSibling;
      case "first-of-type": return elIndex(el, true, false) === 1;
      case "last-of-type": return elIndex(el, true, true) === 1;
      case "only-of-type": return elIndex(el, true, false) === 1 && elIndex(el, true, true) === 1;
      case "nth-child": return nth(p.arg, elIndex(el, false, false));
      case "nth-last-child": return nth(p.arg, elIndex(el, false, true));
      case "nth-of-type": return nth(p.arg, elIndex(el, true, false));
      case "nth-last-of-type": return nth(p.arg, elIndex(el, true, true));
      case "not": return !matchesSel(el, p.arg);
      case "is": case "where": case "matches": case "-webkit-any": case "-moz-any": return matchesSel(el, p.arg);
      case "has": return !!qs(el, p.arg.replace(/^\s*>/, ":scope >"), true);
      case "empty": return el._c.every(function (k) { return k.nodeType !== 1 && !(k.nodeType === 3 && k.data); });
      case "root": return el._p && el._p.nodeType === 9;
      case "scope": return el === SCOPE_EL || (!SCOPE_EL && el._p && el._p.nodeType === 9);
      case "checked": return el.hasAttribute("checked") || el.hasAttribute("selected");
      case "disabled": return el.hasAttribute("disabled");
      case "enabled": return !el.hasAttribute("disabled");
      case "required": return el.hasAttribute("required");
      case "optional": return !el.hasAttribute("required");
      case "link": case "any-link": return (el._n === "a" || el._n === "area") && el.hasAttribute("href");
      case "defined": return true;
      case "lang": return (el.closest("[lang]") || { getAttribute: function () { return ""; } }).getAttribute("lang").toLowerCase().indexOf(String(p.arg).toLowerCase()) === 0;
      default: return false; /* :hover, :focus, ::before... */
    }
  }
  function matchCompound(el, c) {
    if (c.tag && el._n !== c.tag) return false;
    if (c.id !== null && el._a.id !== c.id) return false;
    if (c.cls && !hasClasses(el, c.cls)) return false;
    if (c.attrs) {
      for (var i = 0; i < c.attrs.length; i++) {
        var a = c.attrs[i], v = el._a[a.name];
        if (v === undefined) return false;
        if (!a.op) continue;
        var w = a.val;
        if (a.ci) { v = v.toLowerCase(); w = w.toLowerCase(); }
        switch (a.op) {
          case "=": if (v !== w) return false; break;
          case "~=": if ((" " + v.replace(/\s+/g, " ") + " ").indexOf(" " + w + " ") < 0) return false; break;
          case "|=": if (v !== w && v.indexOf(w + "-") !== 0) return false; break;
          case "^=": if (!w || v.indexOf(w) !== 0) return false; break;
          case "$=": if (!w || v.slice(-w.length) !== w) return false; break;
          case "*=": if (!w || v.indexOf(w) < 0) return false; break;
        }
      }
    }
    if (c.ps) for (var j = 0; j < c.ps.length; j++) if (!matchPseudo(el, c.ps[j])) return false;
    return true;
  }
  function matchGroup(el, g, i) {
    if (!matchCompound(el, g[i].s)) return false;
    if (i === 0) return true;
    var comb = g[i].c, p;
    if (comb === ">") { p = el._p; return !!p && p.nodeType === 1 && matchGroup(p, g, i - 1); }
    if (comb === " ") {
      p = el._p;
      while (p && p.nodeType === 1) { if (matchGroup(p, g, i - 1)) return true; p = p._p; }
      return false;
    }
    if (comb === "+") { p = prevEl(el); return !!p && matchGroup(p, g, i - 1); }
    if (comb === "~") { p = prevEl(el); while (p) { if (matchGroup(p, g, i - 1)) return true; p = prevEl(p); } return false; }
    return false;
  }
  var SCOPE_EL = null;
  function matchesSel(el, sel) {
    var gs = parseSel(sel);
    for (var i = 0; i < gs.length; i++) if (matchGroup(el, gs[i], gs[i].length - 1)) return true;
    return false;
  }
  function qs(root, sel, first) {
    var gs = parseSel(sel), out = [];
    var save = SCOPE_EL;
    SCOPE_EL = root.nodeType === 1 ? root : null;
    try {
      /* #id sozinho: atalho */
      if (gs.length === 1 && gs[0].length === 1) {
        var c = gs[0][0].s;
        if (c.id !== null && !c.tag && !c.cls && !c.attrs && !c.ps) {
          var f = walkEl(root, function (e) { return e._a.id === c.id; });
          return first ? f : (f ? [f] : []);
        }
      }
      var hit = walkEl(root, function (e) {
        for (var i = 0; i < gs.length; i++) {
          if (matchGroup(e, gs[i], gs[i].length - 1)) {
            if (first) return true;
            out.push(e);
            return false;
          }
        }
        return false;
      });
      return first ? hit : out;
    } finally {
      SCOPE_EL = save;
    }
  }

  /* ---------- analisador de HTML ---------- */
  var TOK = /<!--[\s\S]*?(?:-->|$)|<![^>]*>?|<\?[^>]*>?|<\/([a-zA-Z][^\s\/>]*)[^>]*>|<([a-zA-Z][^\s\/>]*)((?:"[^"]*"|'[^']*'|[^>"'])*)>|[^<]+|</g;
  var ATTR = /([^\s"'>\/=]+)(?:\s*=\s*(?:"([^"]*)"|'([^']*)'|([^\s>]+)))?/g;
  var PARSING = false;

  function setAttrs(el, s, onlyNew) {
    ATTR.lastIndex = 0;
    var m;
    while ((m = ATTR.exec(s))) {
      var name = m[1].toLowerCase();
      if (onlyNew && name in el._a) continue;
      var v = m[2] !== undefined ? m[2] : m[3] !== undefined ? m[3] : m[4] !== undefined ? m[4] : "";
      if (!(name in el._a)) el._ao.push(name);
      el._a[name] = decode(v);
    }
  }

  /* html para dentro de ctx (fragment = sem html/head/body proprios) */
  function parseHTML(doc, html, ctx, fragment) {
    var htmlEl = null, head = null, body = null, mode = "body";
    var stack;
    var wasParsing = PARSING;
    PARSING = true;
    if (!fragment) {
      htmlEl = doc.createElement("html"); head = doc.createElement("head"); body = doc.createElement("body");
      doc.appendChild(htmlEl); htmlEl.appendChild(head); htmlEl.appendChild(body);
      stack = [htmlEl, head]; mode = "head";
    } else {
      stack = [ctx];
    }
    var lower = null;
    function top() { return stack[stack.length - 1]; }
    function toBody() { if (mode === "head") { mode = "body"; stack = [htmlEl, body]; } }
    function popTo(i) {
      stack.length = i;
      if (fragment) { if (!stack.length) stack.push(ctx); }
      else if (stack.length < 2) stack = mode === "head" ? [htmlEl, head] : [htmlEl, body];
    }
    function find(names, stops) {
      for (var i = stack.length - 1; i > 0 || (fragment && i >= 0); i--) {
        var t = stack[i]._n;
        if (names[t]) return i;
        if (stops && stops[t]) return -1;
        if (stack[i] === ctx) return -1;
      }
      return -1;
    }
    var P = { p: true }, LI = { li: true }, DTDD = { dt: true, dd: true };
    var STOP_LI = { ul: true, ol: true, menu: true }, STOP_DL = { dl: true };
    var TR = { tr: true }, CELL = { td: true, th: true }, SECT = { thead: true, tbody: true, tfoot: true };
    var STOP_TABLE = { table: true };
    var OPT = { option: true }, OPTG = { option: true, optgroup: true };
    var A = { a: true };
    function closeIf(names, stops) { var k = find(names, stops); if (k >= 0) popTo(k); }

    TOK.lastIndex = 0;
    var m, len = html.length;
    while (TOK.lastIndex < len && (m = TOK.exec(html))) {
      var t = m[0];
      if (m[2] !== undefined) {
        /* inicio de tag */
        var name = m[2].toLowerCase(), attrs = m[3] || "";
        if (name === "html") { if (!fragment) setAttrs(htmlEl, attrs, true); continue; }
        if (name === "head") { continue; }
        if (name === "body") { if (!fragment) { setAttrs(body, attrs, true); toBody(); } continue; }
        if (mode === "head" && !HEAD_OK[name]) toBody();
        if (CLOSE_P[name]) closeIf(P, SCOPE);
        if (name === "li") closeIf(LI, STOP_LI);
        else if (name === "dt" || name === "dd") closeIf(DTDD, STOP_DL);
        else if (name === "option") closeIf(OPT, null);
        else if (name === "optgroup") closeIf(OPTG, null);
        else if (name === "tr") { closeIf(CELL, STOP_TABLE); closeIf(TR, STOP_TABLE); }
        else if (name === "td" || name === "th") closeIf(CELL, STOP_TABLE);
        else if (SECT[name]) { closeIf(CELL, STOP_TABLE); closeIf(TR, STOP_TABLE); closeIf(SECT, STOP_TABLE); }
        else if (name === "a") closeIf(A, SCOPE);
        if (HEADING[name] && HEADING[top()._n]) stack.pop();
        var el = doc.createElement(name);
        setAttrs(el, attrs, false);
        el._parsed = true;              /* nao roda ao ser inserido (innerHTML) */
        if (!fragment) el._main = true; /* script do HTML da pagina: roda na ordem */
        top().appendChild(el);
        var selfClose = /\/\s*$/.test(attrs);
        if (RAWT[name] || RCDATA[name]) {
          if (lower === null) lower = html.toLowerCase();
          var from = TOK.lastIndex, end = lower.indexOf("</" + name, from);
          if (end < 0) end = len;
          var raw = html.slice(from, end);
          if (raw) el.appendChild(doc.createTextNode(RCDATA[name] ? decode(raw) : raw));
          var gt = html.indexOf(">", end);
          TOK.lastIndex = gt < 0 ? len : gt + 1;
          continue;
        }
        if (!VOID[name] && !(selfClose && el instanceof SVGElement)) stack.push(el);
        if (VOID[name] || (selfClose && (name === "svg" || name === "path" || el instanceof SVGElement))) {
          if (stack[stack.length - 1] === el) stack.pop();
        }
        continue;
      }
      if (m[1] !== undefined) {
        /* fim de tag */
        var en = m[1].toLowerCase();
        if (en === "html" || en === "body") { continue; }
        if (en === "head") { if (mode === "head") toBody(); continue; }
        if (en === "br") { top().appendChild(doc.createElement("br")); continue; }
        for (var k = stack.length - 1; k >= 0; k--) {
          if (stack[k]._n === en) { popTo(k); break; }
          if (stack[k] === ctx) break;
        }
        continue;
      }
      var c0 = t.charCodeAt(0), c1 = t.charCodeAt(1);
      if (c0 === 60 && (c1 === 33 || c1 === 63) && t.length > 1) continue; /* <!-- --> <!DOCTYPE> <?xml?> */
      /* texto */
      if (mode === "head") {
        if (/^\s*$/.test(t)) continue;
        toBody();
      }
      var txt = decode(t), parent = top(), last = parent._c[parent._c.length - 1];
      if (last && last.nodeType === 3) last.data += txt;
      else parent.appendChild(doc.createTextNode(txt));
    }
    PARSING = wasParsing;
  }
  function fragment(doc, html, ctx) {
    var f = doc.createDocumentFragment();
    var holder = doc.createElement(ctx && ctx._n ? ctx._n : "div");
    parseHTML(doc, html, holder, true);
    var kids = holder._c.slice();
    for (var i = 0; i < kids.length; i++) f.appendChild(kids[i]);
    return f;
  }

  /* ---------- serializador ---------- */
  var SKIP_OUT = mkset("script noscript template");
  function ser(n, out, final) {
    if (n.nodeType === 3) {
      var p = n._p;
      out.push(p && p.nodeType === 1 && RAWT[p._n] ? n.data : escText(n.data));
      return;
    }
    if (n.nodeType === 8) { if (!final) out.push("<!--" + n.data + "-->"); return; }
    if (n.nodeType !== 1) { serChildren(n, out, final); return; }
    if (final && SKIP_OUT[n._n]) return;
    out.push("<", n._n);
    for (var i = 0; i < n._ao.length; i++) {
      var a = n._ao[i];
      out.push(" ", a, '="', escAttr(n._a[a]), '"');
    }
    out.push(">");
    if (VOID[n._n]) return;
    if (final && n._shadow) serChildren(n._shadow, out, final);
    serChildren(n, out, final);
    out.push("</", n._n, ">");
  }
  function serChildren(n, out, final) {
    var c = n._c;
    for (var i = 0; i < c.length; i++) ser(c[i], out, final);
  }

  /* ---------- eventos ---------- */
  function addEL(t, type, fn, opt) {
    if (!fn) return;
    var L = t._ev || (t._ev = Object.create(null));
    var list = L[type] || (L[type] = []);
    for (var i = 0; i < list.length; i++) if (list[i].fn === fn) return;
    list.push({ fn: fn, once: !!(opt && typeof opt === "object" && opt.once) });
  }
  function removeEL(t, type, fn) {
    var list = t._ev && t._ev[type];
    if (!list) return;
    for (var i = 0; i < list.length; i++) if (list[i].fn === fn) { list.splice(i, 1); return; }
  }
  function fire(t, ev) {
    ev.currentTarget = t;
    var list = t._ev && t._ev[ev.type];
    if (list) {
      list = list.slice();
      for (var i = 0; i < list.length && !ev._stopImm; i++) {
        var l = list[i];
        if (l.once) removeEL(t, ev.type, l.fn);
        try {
          if (typeof l.fn === "function") l.fn.call(t, ev);
          else if (l.fn && typeof l.fn.handleEvent === "function") l.fn.handleEvent(ev);
        } catch (e) { report(e, "evento " + ev.type); }
      }
    }
    var h = t["on" + ev.type];
    if (typeof h === "function" && !ev._stopImm) {
      try { if (h.call(t, ev) === false) ev.preventDefault(); } catch (e) { report(e, "on" + ev.type); }
    }
  }
  function dispatch(target, ev) {
    if (!(ev instanceof Event)) throw new TypeError("dispatchEvent: nao e Event");
    ev.target = target;
    ev.eventPhase = 2;
    fire(target, ev);
    if (ev.bubbles && !ev._stop) {
      ev.eventPhase = 3;
      var n = target === G ? null : target._p;
      while (n && !ev._stop) { fire(n, ev); n = n._p; }
      if (!ev._stop && target !== G && target.nodeType !== undefined && target.isConnected) fire(G, ev);
    }
    ev.eventPhase = 0;
    ev.currentTarget = null;
    drain();
    return !ev.defaultPrevented;
  }
  class Event {
    constructor(type, init) {
      init = init || {};
      this.type = String(type);
      this.bubbles = !!init.bubbles; this.cancelable = !!init.cancelable; this.composed = !!init.composed;
      this.defaultPrevented = false; this.target = null; this.currentTarget = null;
      this.eventPhase = 0; this.isTrusted = false; this.timeStamp = vnow;
      this._stop = false; this._stopImm = false;
    }
    get srcElement() { return this.target; }
    get returnValue() { return !this.defaultPrevented; }
    set returnValue(v) { if (!v) this.preventDefault(); }
    get cancelBubble() { return this._stop; }
    set cancelBubble(v) { if (v) this._stop = true; }
    composedPath() { var p = [], n = this.target; while (n) { p.push(n); n = n._p; } p.push(G); return p; }
    preventDefault() { if (this.cancelable) this.defaultPrevented = true; }
    stopPropagation() { this._stop = true; }
    stopImmediatePropagation() { this._stop = this._stopImm = true; }
    initEvent(t, b, c) { this.type = String(t); this.bubbles = !!b; this.cancelable = !!c; }
  }
  Event.NONE = 0; Event.CAPTURING_PHASE = 1; Event.AT_TARGET = 2; Event.BUBBLING_PHASE = 3;
  class CustomEvent extends Event {
    constructor(t, i) { super(t, i); this.detail = i && i.detail !== undefined ? i.detail : null; }
    initCustomEvent(t, b, c, d) { this.initEvent(t, b, c); this.detail = d; }
  }
  class UIEvent extends Event { constructor(t, i) { super(t, i); this.detail = (i && i.detail) || 0; this.view = G; } }
  class MouseEvent extends UIEvent {
    constructor(t, i) {
      super(t, i); i = i || {};
      this.clientX = i.clientX || 0; this.clientY = i.clientY || 0; this.screenX = 0; this.screenY = 0;
      this.pageX = this.clientX; this.pageY = this.clientY; this.button = i.button || 0; this.buttons = 0;
      this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey;
      this.relatedTarget = i.relatedTarget || null;
    }
  }
  class KeyboardEvent extends UIEvent {
    constructor(t, i) {
      super(t, i); i = i || {};
      this.key = i.key || ""; this.code = i.code || ""; this.keyCode = i.keyCode || 0; this.which = this.keyCode;
      this.ctrlKey = !!i.ctrlKey; this.shiftKey = !!i.shiftKey; this.altKey = !!i.altKey; this.metaKey = !!i.metaKey; this.repeat = false;
    }
  }
  class FocusEvent extends UIEvent {}
  class InputEvent extends UIEvent {}
  class PointerEvent extends MouseEvent {}
  class WheelEvent extends MouseEvent {}
  class TouchEvent extends UIEvent {}
  class MessageEvent extends Event { constructor(t, i) { super(t, i); this.data = i && i.data; this.origin = (i && i.origin) || ""; } }
  class ProgressEvent extends Event { constructor(t, i) { super(t, i); this.loaded = 0; this.total = 0; this.lengthComputable = false; } }
  class ErrorEvent extends Event { constructor(t, i) { super(t, i); this.message = (i && i.message) || ""; this.error = i && i.error; } }
  class PopStateEvent extends Event { constructor(t, i) { super(t, i); this.state = i && i.state; } }
  class HashChangeEvent extends Event {}
  class StorageEvent extends Event {}
  class AnimationEvent extends Event {}
  class TransitionEvent extends Event {}

  /* ---------- documento ---------- */
  class Document extends Node {
    get nodeName() { return "#document"; }
    get documentElement() { return this.firstElementChild; }
    get head() { var h = this.documentElement; return h ? h.querySelector("head") : null; }
    get body() {
      var h = this.documentElement; if (!h) return null;
      var c = h._c; for (var i = 0; i < c.length; i++) if (c[i]._n === "body" || c[i]._n === "frameset") return c[i];
      return null;
    }
    set body(b) { var old = this.body, h = this.documentElement; if (!h) return; if (old) h.replaceChild(b, old); else h.appendChild(b); }
    get title() { var t = this.querySelector("title"); return t ? t.textContent.replace(/\s+/g, " ").trim() : ""; }
    set title(v) {
      var t = this.querySelector("title");
      if (!t) { t = this.createElement("title"); if (this.head) this.head.appendChild(t); }
      t.textContent = v;
    }
    createElement(name) {
      name = String(name).toLowerCase();
      var C = TAGCLASS[name] ? CLASSES["HTML" + TAGCLASS[name] + "Element"] : null;
      if (!C) C = SVG_TAGS[name] ? SVGElement : HTMLElement;
      var el = new C();
      initNode(el, 1, this);
      el._n = name; el._a = Object.create(null); el._ao = [];
      return el;
    }
    createElementNS(ns, name) {
      var local = String(name).replace(/^.*:/, "");
      var el = this.createElement(local);
      if (ns && ns !== "http://www.w3.org/1999/xhtml") {
        Object.setPrototypeOf(el, SVGElement.prototype);
        el._n = local; el._ns = ns;
      }
      return el;
    }
    createTextNode(s) { var t = new Text(); initNode(t, 3, this); t.data = String(s); return t; }
    createComment(s) { var t = new Comment(); initNode(t, 8, this); t.data = String(s); return t; }
    createDocumentFragment() { var f = new DocumentFragment(); initNode(f, 11, this); return f; }
    createEvent(kind) { return /custom/i.test(kind) ? new CustomEvent("") : /mouse/i.test(kind) ? new MouseEvent("") : new Event(""); }
    createRange() { return new Range_(this); }
    createTreeWalker(root, what, filter) { return new TreeWalker_(root, what, filter); }
    createNodeIterator(root, what, filter) { return new TreeWalker_(root, what, filter); }
    createAttribute(n) { return { name: n, value: "" }; }
    importNode(n, deep) { return n.cloneNode(deep); }
    adoptNode(n) { if (n._p) n._p._detach(n); return n; }
    getElementById(id) { id = String(id); return walkEl(this, function (e) { return e._a.id === id; }); }
    getElementsByName(n) { var out = []; walkEl(this, function (e) { if (e._a.name === n) out.push(e); }); return out; }
    hasFocus() { return true; }
    get activeElement() { return this._active || this.body; }
    get defaultView() { return G; }
    get location() { return LOC; }
    set location(v) { LOC.href = v; }
    get URL() { return LOC.href; }
    get documentURI() { return LOC.href; }
    get domain() { return LOC.hostname; }
    get referrer() { return ""; }
    get cookie() { var o = this._ck; return Object.keys(o).map(function (k) { return k + "=" + o[k]; }).join("; "); }
    set cookie(v) {
      var s = String(v).split(";")[0], i = s.indexOf("=");
      if (i <= 0) return;
      var k = s.slice(0, i).trim(), val = s.slice(i + 1).trim();
      if (/;\s*expires=Thu, 01 Jan 1970|;\s*max-age=0/i.test(v)) delete this._ck[k]; else this._ck[k] = val;
    }
    get readyState() { return this._rs; }
    get visibilityState() { return "visible"; }
    get hidden() { return false; }
    get compatMode() { return "CSS1Compat"; }
    get characterSet() { return "UTF-8"; }
    get charset() { return "UTF-8"; }
    get inputEncoding() { return "UTF-8"; }
    get contentType() { return "text/html"; }
    get doctype() { return { name: "html", publicId: "", systemId: "" }; }
    get currentScript() { return this._cur || null; }
    get scripts() { return this.getElementsByTagName("script"); }
    get images() { return this.getElementsByTagName("img"); }
    get forms() { return this.getElementsByTagName("form"); }
    get links() { return this.querySelectorAll("a[href],area[href]"); }
    get anchors() { return this.querySelectorAll("a[name]"); }
    get styleSheets() { return []; }
    get fonts() { return FONTS; }
    get implementation() { return { createHTMLDocument: function (t) { var d = newDocument("about:blank"); parseHTML(d, "<title>" + escText(t || "") + "</title>", null, false); return d; }, hasFeature: function () { return true; } }; }
    get scrollingElement() { return this.documentElement; }
    get fullscreenElement() { return null; }
    get pointerLockElement() { return null; }
    get designMode() { return "off"; }
    set designMode(v) {}
    get dir() { var h = this.documentElement; return h ? h.getAttribute("dir") || "" : ""; }
    get firstElementChild() { var c = this._c; for (var i = 0; i < c.length; i++) if (c[i].nodeType === 1) return c[i]; return null; }
    write() {
      var s = Array.prototype.join.call(arguments, "");
      if (this._cur) this._wbuf.push(s);
      else if (this._rs === "loading") { var b = this.body || this.documentElement; if (b) b.appendChild(fragment(this, s, b)); }
      else log("document.write depois do load ignorado");
    }
    writeln() { this.write(Array.prototype.join.call(arguments, "") + "\n"); }
    open() { return this; }
    close() {}
    execCommand() { return false; }
    queryCommandSupported() { return false; }
    getSelection() { return G.getSelection(); }
    elementFromPoint() { return null; }
    elementsFromPoint() { return []; }
    exitFullscreen() { return Promise.resolve(); }
  }
  class HTMLDocument extends Document {}
  var SVG_TAGS = mkset("svg g path circle rect line polyline polygon ellipse text tspan defs use symbol lineargradient radialgradient stop clippath mask pattern image foreignobject title desc marker filter");
  var FONTS = {
    ready: Promise.resolve(), status: "loaded", size: 0,
    load: function () { return Promise.resolve([]); }, check: function () { return true; },
    add: function () {}, delete: function () {}, forEach: function () {},
    addEventListener: function () {}, removeEventListener: function () {}
  };
  function newDocument(url) {
    var d = new HTMLDocument();
    initNode(d, 9, d);
    d._ck = Object.create(null); d._rs = "loading"; d._wbuf = []; d._cur = null; d._base = url;
    return d;
  }

  class Range_ {
    constructor(doc) { this._doc = doc; this.collapsed = true; this.startContainer = doc; this.startOffset = 0; this.endContainer = doc; this.endOffset = 0; this.commonAncestorContainer = doc; }
    setStart(n, o) { this.startContainer = n; this.startOffset = o; }
    setEnd(n, o) { this.endContainer = n; this.endOffset = o; }
    setStartBefore() {} setStartAfter() {} setEndBefore() {} setEndAfter() {}
    selectNode(n) { this.startContainer = this.endContainer = n; }
    selectNodeContents(n) { this.startContainer = this.endContainer = n; }
    collapse() {} detach() {}
    cloneRange() { return new Range_(this._doc); }
    createContextualFragment(html) { return fragment(this._doc, String(html), this._doc.body); }
    getBoundingClientRect() { return rect0(); }
    getClientRects() { return []; }
    deleteContents() {} extractContents() { return this._doc.createDocumentFragment(); }
    insertNode(n) { if (this.startContainer && this.startContainer.appendChild) this.startContainer.appendChild(n); }
    toString() { return ""; }
  }
  class TreeWalker_ {
    constructor(root, what, filter) {
      this.root = root; this.whatToShow = what === undefined ? 0xFFFFFFFF : what; this.filter = filter || null;
      this.currentNode = root; this._list = null; this._i = 0;
    }
    _all() {
      if (!this._list) {
        var out = [], self = this;
        walk(this.root, function (n) {
          if (n === self.root) return;
          var bit = 1 << (n.nodeType - 1);
          if (!(self.whatToShow & bit)) return;
          var f = self.filter, r = 1;
          if (f) r = typeof f === "function" ? f(n) : f.acceptNode(n);
          if (r === 1) out.push(n);
        });
        this._list = out;
      }
      return this._list;
    }
    nextNode() { var l = this._all(); if (this._i >= l.length) return null; return (this.currentNode = l[this._i++]); }
    previousNode() { var l = this._all(); if (this._i <= 1) return null; this._i--; return (this.currentNode = l[this._i - 1]); }
    firstChild() { return this.nextNode(); }
    parentNode() { return null; }
    nextSibling() { return this.nextNode(); }
    detach() {}
  }
  var NodeFilter = { FILTER_ACCEPT: 1, FILTER_REJECT: 2, FILTER_SKIP: 3, SHOW_ALL: 0xFFFFFFFF,
    SHOW_ELEMENT: 1, SHOW_TEXT: 4, SHOW_COMMENT: 128, SHOW_DOCUMENT: 256 };

  /* ---------- location ---------- */
  class Location {
    get href() { return fmtURL(PAGE); }
    set href(v) { go(v); }
    get protocol() { return PAGE.protocol; }
    get host() { return host(PAGE); }
    get hostname() { return PAGE.hostname; }
    get port() { return PAGE.port; }
    get pathname() { return PAGE.pathname; }
    set pathname(v) { var u = Object.assign({}, PAGE); u.pathname = String(v); go(fmtURL(u)); }
    get search() { return PAGE.search; }
    set search(v) { var u = Object.assign({}, PAGE); u.search = v ? (String(v).charAt(0) === "?" ? v : "?" + v) : ""; go(fmtURL(u)); }
    get hash() { return PAGE.hash; }
    set hash(v) { PAGE.hash = v ? (String(v).charAt(0) === "#" ? v : "#" + v) : ""; }
    get origin() { return origin(PAGE); }
    get ancestorOrigins() { return []; }
    assign(u) { go(u); }
    replace(u) { go(u); }
    reload() { log("location.reload() ignorado"); }
    toString() { return this.href; }
  }
  function go(u) {
    var abs = resolve(String(u), fmtURL(PAGE));
    var a = parseURL(abs), b = PAGE;
    if (a && a.protocol === b.protocol && host(a) === host(b) && a.pathname === b.pathname && a.search === b.search) {
      PAGE.hash = a.hash;
      return;
    }
    if (/^javascript:/i.test(abs)) return;
    if (!IR) IR = abs;
    log("a pagina pediu para ir a " + abs);
  }

  /* ---------- URL e URLSearchParams ---------- */
  function encQ(s) {
    return encodeURIComponent(s).replace(/%20/g, "+").replace(/[!'()~]/g, function (c) {
      return "%" + c.charCodeAt(0).toString(16).toUpperCase();
    });
  }
  function decQ(s) { try { return decodeURIComponent(String(s).replace(/\+/g, " ")); } catch (e) { return s; } }
  class URLSearchParams {
    constructor(init) {
      this._l = []; this._u = null;
      if (init == null) return;
      if (typeof init === "string") this._parse(init);
      else if (init instanceof URLSearchParams) this._l = init._l.map(function (p) { return p.slice(); });
      else if (typeof init[Symbol.iterator] === "function") { for (var p of init) this._l.push([String(p[0]), String(p[1])]); }
      else Object.keys(init).forEach(function (k) { this._l.push([k, String(init[k])]); }, this);
    }
    _parse(s) {
      s = String(s);
      if (s.charAt(0) === "?") s = s.slice(1);
      this._l = s ? s.split("&").filter(Boolean).map(function (kv) {
        var i = kv.indexOf("=");
        return i < 0 ? [decQ(kv), ""] : [decQ(kv.slice(0, i)), decQ(kv.slice(i + 1))];
      }) : [];
    }
    _sync() { if (this._u) { var q = this.toString(); this._u._p.search = q ? "?" + q : ""; } }
    get size() { return this._l.length; }
    append(k, v) { this._l.push([String(k), String(v)]); this._sync(); }
    delete(k) { k = String(k); this._l = this._l.filter(function (p) { return p[0] !== k; }); this._sync(); }
    get(k) { k = String(k); for (var i = 0; i < this._l.length; i++) if (this._l[i][0] === k) return this._l[i][1]; return null; }
    getAll(k) { k = String(k); return this._l.filter(function (p) { return p[0] === k; }).map(function (p) { return p[1]; }); }
    has(k) { return this.get(k) !== null; }
    set(k, v) {
      k = String(k); v = String(v);
      var done = false;
      this._l = this._l.filter(function (p) { if (p[0] !== k) return true; if (done) return false; p[1] = v; done = true; return true; });
      if (!done) this._l.push([k, v]);
      this._sync();
    }
    sort() { this._l.sort(function (a, b) { return a[0] < b[0] ? -1 : a[0] > b[0] ? 1 : 0; }); this._sync(); }
    forEach(fn, t) { this._l.forEach(function (p) { fn.call(t, p[1], p[0], this); }, this); }
    keys() { return this._l.map(function (p) { return p[0]; })[Symbol.iterator](); }
    values() { return this._l.map(function (p) { return p[1]; })[Symbol.iterator](); }
    entries() { return this._l.map(function (p) { return [p[0], p[1]]; })[Symbol.iterator](); }
    [Symbol.iterator]() { return this.entries(); }
    toString() { return this._l.map(function (p) { return encQ(p[0]) + "=" + encQ(p[1]); }).join("&"); }
  }
  class URL_ {
    constructor(u, base) {
      var abs = base !== undefined ? resolve(String(u), String(base instanceof URL_ ? base.href : base)) : String(u);
      var p = parseURL(abs);
      if (!p || (base === undefined && !/^[a-zA-Z][a-zA-Z0-9+.\-]*:/.test(String(u).trim()))) throw new TypeError("URL invalida: " + u);
      this._p = p;
      this._sp = null;
    }
    get href() { return fmtURL(this._p); }
    set href(v) { var p = parseURL(v); if (p) { this._p = p; this._sp = null; } }
    get protocol() { return this._p.protocol; }
    set protocol(v) { this._p.protocol = String(v).replace(/:?$/, ":"); }
    get host() { return host(this._p); }
    set host(v) { var a = String(v).split(":"); this._p.hostname = a[0]; this._p.port = a[1] || ""; }
    get hostname() { return this._p.hostname; }
    set hostname(v) { this._p.hostname = String(v); }
    get port() { return this._p.port; }
    set port(v) { this._p.port = String(v); }
    get pathname() { return this._p.pathname; }
    set pathname(v) { v = String(v); this._p.pathname = v.charAt(0) === "/" ? v : "/" + v; }
    get search() { return this._p.search; }
    set search(v) { v = String(v); this._p.search = v && v !== "?" ? (v.charAt(0) === "?" ? v : "?" + v) : ""; this._sp = null; }
    get hash() { return this._p.hash; }
    set hash(v) { v = String(v); this._p.hash = v && v !== "#" ? (v.charAt(0) === "#" ? v : "#" + v) : ""; }
    get origin() { return origin(this._p); }
    get username() { return this._p.username; }
    get password() { return this._p.password; }
    get searchParams() {
      if (!this._sp) { this._sp = new URLSearchParams(this._p.search); this._sp._u = this; }
      return this._sp;
    }
    toString() { return this.href; }
    toJSON() { return this.href; }
  }
  URL_.createObjectURL = function () { return "blob:sodos/" + (++BLOBS); };
  URL_.revokeObjectURL = function () {};
  URL_.canParse = function (u, b) { try { new URL_(u, b); return true; } catch (e) { return false; } };
  var BLOBS = 0;

  /* ---------- timers ---------- */
  var vnow = 0, timers = [], tseq = 1, nTimers = 0;
  function addTimer(fn, ms, args, rep) {
    var id = tseq++;
    ms = Math.max(0, +ms || 0);
    timers.push({ id: id, at: vnow + ms, fn: fn, args: args, rep: rep ? Math.max(4, ms) : 0, n: 0 });
    return id;
  }
  function clearTimer(id) {
    for (var i = 0; i < timers.length; i++) if (timers[i].id === id) { timers.splice(i, 1); return; }
  }
  function callTimer(t) {
    try {
      if (typeof t.fn === "function") t.fn.apply(G, t.args);
      else if (typeof t.fn === "string") runCode(t.fn, "timer");
    } catch (e) { report(e, "timer"); }
    drain();
  }
  function runTimers() {
    while (timers.length && nTimers < TIMERS_MAX && dateNow_() - T0 < BUDGET_MS) {
      var bi = 0;
      for (var i = 1; i < timers.length; i++) if (timers[i].at < timers[bi].at) bi = i;
      var t = timers[bi];
      if (t.at > VIRT_MAX) break;
      if (t.at > vnow) vnow = t.at;
      if (t.rep && t.n < 15) { t.at += t.rep; t.n++; } else timers.splice(bi, 1);
      nTimers++;
      callTimer(t);
    }
    if (timers.length && dateNow_() - T0 >= BUDGET_MS) log("tempo esgotado com " + timers.length + " timers pendentes");
  }

  /* ---------- scripts ---------- */
  var MANIFEST = Object.create(null), NSCRIPT = 0, NEXT = 0;
  function jsType(el) {
    var t = (el.getAttribute("type") || "").trim().toLowerCase();
    if (!t) return "js";
    if (/^(text|application)\/(x-)?(java|ecma)script$|^text\/jscript$|^text\/livescript$|^(application|text)\/javascript;.*$/.test(t)) return "js";
    if (t === "module") return "module";
    return "";
  }
  function runCode(code, name) {
    try { evaluate_(code, { fileName: name, lineNumber: 1 }); }
    catch (e) { report(e, name); }
  }
  function runScript(el) {
    if (el._done) return;
    el._done = true;
    var kind = jsType(el);
    if (kind === "module") { log("script module nao suportado: " + short(el.getAttribute("src") || "inline")); return; }
    if (!kind) return;
    var src = el.getAttribute("src"), code, name;
    var file = null;
    if (src !== null) {
      var abs = resolve(src, DOC._base);
      file = MANIFEST[src] || MANIFEST[abs];
      if (!file) {
        if (FALTA.indexOf(abs) < 0 && /^https?:/.test(abs)) FALTA.push(abs);
        dispatch(el, new Event("error"));
        return;
      }
      name = abs;
      NEXT++;
    } else {
      code = el.textContent;
      name = "inline" + (++NSCRIPT);
      if (!/\S/.test(code)) return;
    }
    var prev = DOC._cur, prevBuf = DOC._wbuf, ts = dateNow_();
    DOC._cur = el; DOC._wbuf = [];
    if (file) {
      try { load_(file); } catch (e) { report(e, name); }
    } else {
      runCode(code, name);
    }
    if (DEBUG) mark("script " + short(name) + " (" + (file ? "externo" : code.length + " chars") + ") levou " + (dateNow_() - ts) + " ms");
    var written = DOC._wbuf.join("");
    DOC._cur = prev; DOC._wbuf = prevBuf;
    drain();
    if (written && el._p) {
      var f2 = fragment(DOC, written, el._p);
      var news = [];
      walk(f2, function (n) { if (n.nodeType === 1 && n._n === "script") news.push(n); });
      var inHead = DOC.head && DOC.head.contains(el) && DOC.body;
      var kids = f2._c.slice(), after = el;
      for (var i = 0; i < kids.length; i++) {
        var k = kids[i];
        var headOk = k.nodeType === 1 ? HEAD_OK[k._n] : !/\S/.test(k.data || "");
        if (inHead && !headOk) {
          /* no head, o que nao e de head abre o body: vai para o comeco dele */
          var b = DOC.body, ref = DOC._hw && DOC._hw._p === b ? DOC._hw.nextSibling : b.firstChild;
          b.insertBefore(k, ref);
          DOC._hw = k;
        } else {
          el._p.insertBefore(k, after.nextSibling);
          after = k;
        }
      }
      for (var j = 0; j < news.length; j++) runScript(news[j]);
    }
    if (src !== null) dispatch(el, new Event("load"));
  }
  /* no inserido depois da carga: script dinamico roda na hora */
  function inserted(n) {
    if (n.nodeType !== 1) return;
    if (n._n === "script") { if (!n._parsed && n.isConnected && PHASE > 0) runScript(n); return; }
    if (n._c.length && PHASE > 0 && n.isConnected) {
      var list = [];
      walkEl(n, function (e) { if (e._n === "script" && !e._parsed && !e._done) list.push(e); });
      for (var i = 0; i < list.length; i++) runScript(list[i]);
    }
  }

  /* ---------- janela ---------- */
  function Storage_() {
    var data = Object.create(null);
    var s = {
      getItem: function (k) { k = String(k); return k in data ? data[k] : null; },
      setItem: function (k, v) { data[String(k)] = String(v); },
      removeItem: function (k) { delete data[String(k)]; },
      clear: function () { data = Object.create(null); },
      key: function (i) { return Object.keys(data)[i] || null; }
    };
    Object.defineProperty(s, "length", { get: function () { return Object.keys(data).length; } });
    return s;
  }
  var B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  function btoa_(s) {
    s = String(s);
    var out = "", i = 0;
    for (; i < s.length; i += 3) {
      var a = s.charCodeAt(i), b = s.charCodeAt(i + 1), c = s.charCodeAt(i + 2);
      if (a > 255 || b > 255 || c > 255) throw new Error("btoa: caractere fora do Latin-1");
      var n = (a << 16) | ((b || 0) << 8) | (c || 0);
      out += B64.charAt(n >> 18) + B64.charAt((n >> 12) & 63) +
        (i + 1 < s.length ? B64.charAt((n >> 6) & 63) : "=") + (i + 2 < s.length ? B64.charAt(n & 63) : "=");
    }
    return out;
  }
  function atob_(s) {
    s = String(s).replace(/[\s=]+/g, "");
    var out = "", bits = 0, val = 0;
    for (var i = 0; i < s.length; i++) {
      var d = B64.indexOf(s.charAt(i));
      if (d < 0) d = s.charAt(i) === "-" ? 62 : s.charAt(i) === "_" ? 63 : -1;
      if (d < 0) throw new Error("atob: base64 invalido");
      val = (val << 6) | d; bits += 6;
      if (bits >= 8) { bits -= 8; out += String.fromCharCode((val >> bits) & 255); }
    }
    return out;
  }
  class TextEncoder_ {
    get encoding() { return "utf-8"; }
    encode(s) {
      s = unescape(encodeURIComponent(String(s === undefined ? "" : s)));
      var a = new Uint8Array(s.length);
      for (var i = 0; i < s.length; i++) a[i] = s.charCodeAt(i);
      return a;
    }
  }
  class TextDecoder_ {
    constructor(label) { this.encoding = label || "utf-8"; }
    decode(buf) {
      if (!buf) return "";
      var a = buf instanceof Uint8Array ? buf : new Uint8Array(buf.buffer || buf);
      var s = "";
      for (var i = 0; i < a.length; i++) s += String.fromCharCode(a[i]);
      if (/latin|8859|1252/i.test(this.encoding)) return s;
      try { return decodeURIComponent(escape(s)); } catch (e) { return s; }
    }
  }
  class XMLHttpRequest_ extends EventTarget_ {
    constructor() { super(); this.readyState = 0; this.status = 0; this.statusText = ""; this.responseText = ""; this.response = ""; this.responseType = ""; this.timeout = 0; this.withCredentials = false; this.upload = new EventTarget_(); }
    open(m, u) { this._url = resolve(u, DOC._base); this.readyState = 1; }
    setRequestHeader() {}
    getResponseHeader() { return null; }
    getAllResponseHeaders() { return ""; }
    overrideMimeType() {}
    abort() {}
    send() {
      var x = this;
      log("XMLHttpRequest sem rede: " + short(this._url));
      addTimer(function () {
        x.readyState = 4;
        dispatch(x, new Event("readystatechange"));
        dispatch(x, new ProgressEvent("error"));
        dispatch(x, new ProgressEvent("loadend"));
      }, 0, [], false);
    }
  }
  XMLHttpRequest_.UNSENT = 0; XMLHttpRequest_.OPENED = 1; XMLHttpRequest_.DONE = 4;
  function noNet(kind) {
    return class extends EventTarget_ {
      constructor(u) { super(); log(kind + " sem rede: " + short(u)); this.readyState = 3; var self = this; addTimer(function () { dispatch(self, new Event("error")); }, 0, [], false); }
      send() {} close() {}
    };
  }
  class Observer_ { constructor(cb) { this._cb = cb; } observe() {} unobserve() {} disconnect() {} takeRecords() { return []; } }
  class IntersectionObserver_ extends Observer_ {
    observe(el) {
      /* tudo "visivel": imagens e listas preguicosas carregam na foto */
      var self = this, r = rect0();
      addTimer(function () {
        try { self._cb([{ target: el, isIntersecting: true, intersectionRatio: 1, boundingClientRect: r, intersectionRect: r, rootBounds: null, time: vnow }], self); }
        catch (e) { report(e, "IntersectionObserver"); }
      }, 0, [], false);
    }
  }
  function matchMedia_(q) {
    q = String(q);
    var W = INNER_W, ok = true;
    q.replace(/\(\s*(min|max)-width\s*:\s*([\d.]+)(px|em|rem)?\s*\)/gi, function (m, mm, v, unit) {
      var px = parseFloat(v) * (unit && unit !== "px" ? 16 : 1);
      if (mm.toLowerCase() === "min" ? W < px : W > px) ok = false;
      return m;
    });
    if (/prefers-color-scheme\s*:\s*dark|prefers-reduced-motion\s*:\s*reduce|\bprint\b|hover\s*:\s*none|pointer\s*:\s*coarse/i.test(q)) ok = false;
    return { matches: ok, media: q, onchange: null, addListener: function () {}, removeListener: function () {},
      addEventListener: function () {}, removeEventListener: function () {}, dispatchEvent: function () { return true; } };
  }

  /* ---------- montagem ---------- */
  var DOC = null, LOC = null, PAGE = null, PHASE = 0, INNER_W = 1024, INNER_H = 768;
  var DADOS = {};
  try {
    load_("DADOS.JS");
    DADOS = G.__BRDADOS || {};
    G.__BRDADOS = undefined; /* var global: nao da para apagar */
  } catch (e) { report(e, "DADOS.JS"); }
  var PAGE_URL = DADOS.url || "about:blank";
  if (+DADOS.w > 0) INNER_W = +DADOS.w;
  if (+DADOS.h > 0) INNER_H = +DADOS.h;
  if (DADOS.scripts) Object.keys(DADOS.scripts).forEach(function (k) { MANIFEST[k] = DADOS.scripts[k]; });
  PAGE = parseURL(PAGE_URL) || parseURL("about:blank");
  LOC = new Location();
  DOC = newDocument(PAGE_URL);

  var ALL = {
    window: G, self: G, top: G, parent: G, frames: G, globalThis: G, frameElement: null, opener: null,
    closed: false, name: "", length: 0, origin: origin(PAGE), isSecureContext: PAGE.protocol === "https:",
    document: DOC, navigator: {
      userAgent: "Mozilla/5.0 (SODOS; x86_64) SO-Navegador/1.0 SpiderMonkey",
      appName: "Netscape", appCodeName: "Mozilla", appVersion: "5.0 (SODOS)", platform: "SODOS",
      product: "Gecko", productSub: "20100101", vendor: "", vendorSub: "", language: "pt-BR",
      languages: ["pt-BR", "pt", "en"], cookieEnabled: true, onLine: true, doNotTrack: "1",
      hardwareConcurrency: 1, maxTouchPoints: 0, deviceMemory: 1, webdriver: false, pdfViewerEnabled: false,
      plugins: [], mimeTypes: [], javaEnabled: function () { return false; },
      sendBeacon: function (u) { log("sendBeacon sem rede: " + short(u)); return true; },
      vibrate: function () { return false; }, registerProtocolHandler: function () {},
      permissions: { query: function () { return Promise.resolve({ state: "denied", onchange: null }); } },
      mediaDevices: undefined, clipboard: undefined, geolocation: undefined, serviceWorker: undefined
    },
    screen: { width: INNER_W, height: INNER_H, availWidth: INNER_W, availHeight: INNER_H, colorDepth: 24, pixelDepth: 24,
      orientation: { type: "landscape-primary", angle: 0, addEventListener: function () {} } },
    innerWidth: INNER_W, innerHeight: INNER_H, outerWidth: INNER_W, outerHeight: INNER_H,
    devicePixelRatio: 1, scrollX: 0, scrollY: 0, pageXOffset: 0, pageYOffset: 0, screenX: 0, screenY: 0,
    screenLeft: 0, screenTop: 0,
    scrollTo: function () {}, scrollBy: function () {}, scroll: function () {},
    moveTo: function () {}, resizeTo: function () {}, focus: function () {}, blur: function () {},
    stop: function () {}, print: function () { log("window.print() ignorado"); },
    open: function (u) { log("window.open ignorado: " + short(u)); return null; },
    close: function () {}, postMessage: function (d) { var e = new MessageEvent("message", { data: d, origin: origin(PAGE) }); addTimer(function () { dispatch(G, e); }, 0, [], false); },
    alert: function (m) { LOG.push("[js-alert] " + String(m).replace(/\s+/g, " ").slice(0, 200)); },
    confirm: function () { return false; }, prompt: function () { return null; },
    getComputedStyle: function (el) { return el && el.style ? el.style : makeStyle(DOC.createElement("div")); },
    matchMedia: matchMedia_,
    getSelection: function () { return { rangeCount: 0, isCollapsed: true, type: "None", toString: function () { return ""; }, removeAllRanges: function () {}, addRange: function () {}, getRangeAt: function () { return new Range_(DOC); }, collapse: function () {}, selectAllChildren: function () {} }; },
    localStorage: Storage_(), sessionStorage: Storage_(),
    history: { length: 1, state: null, scrollRestoration: "auto",
      pushState: function (s) { this.state = s; }, replaceState: function (s) { this.state = s; },
      back: function () {}, forward: function () {}, go: function () {} },
    console: (function () {
      var c = {};
      ["log", "info", "debug", "trace", "dir", "dirxml", "table", "group", "groupCollapsed", "groupEnd",
        "time", "timeEnd", "timeLog", "count", "countReset", "profile", "profileEnd", "timeStamp", "clear"].forEach(function (k) { c[k] = function () {}; });
      c.warn = function () { log("console.warn: " + Array.prototype.map.call(arguments, String).join(" ")); };
      c.error = function () { log("console.error: " + Array.prototype.map.call(arguments, String).join(" ")); };
      c.assert = function (ok) { if (!ok) log("console.assert falhou"); };
      return c;
    })(),
    setTimeout: function (fn, ms) { return addTimer(fn, ms, Array.prototype.slice.call(arguments, 2), false); },
    setInterval: function (fn, ms) { return addTimer(fn, ms, Array.prototype.slice.call(arguments, 2), true); },
    clearTimeout: clearTimer, clearInterval: clearTimer,
    setImmediate: function (fn) { return addTimer(fn, 0, Array.prototype.slice.call(arguments, 1), false); },
    clearImmediate: clearTimer,
    requestAnimationFrame: function (fn) { return addTimer(function () { fn(vnow); }, 16, [], false); },
    cancelAnimationFrame: clearTimer,
    requestIdleCallback: function (fn) { return addTimer(function () { fn({ didTimeout: false, timeRemaining: function () { return 10; } }); }, 1, [], false); },
    cancelIdleCallback: clearTimer,
    queueMicrotask: function (fn) { Promise.resolve().then(fn); },
    performance: {
      /* relogio simulado + o tempo real: um laco de espera ainda termina */
      now: function () { return vnow + (dateNow_() - T0); }, timeOrigin: T0,
      timing: { navigationStart: T0, domContentLoadedEventEnd: T0, loadEventEnd: T0, responseStart: T0, fetchStart: T0 },
      mark: function () {}, measure: function () {}, clearMarks: function () {}, clearMeasures: function () {},
      getEntries: function () { return []; }, getEntriesByType: function () { return []; },
      getEntriesByName: function () { return []; }, navigation: { type: 0 },
      memory: { usedJSHeapSize: 0, totalJSHeapSize: 0, jsHeapSizeLimit: 0 }
    },
    fetch: function (u) {
      var url = u && u.url ? u.url : String(u);
      log("fetch sem rede: " + short(url));
      return Promise.reject(new TypeError("NetworkError: o JS.EXE nao tem rede"));
    },
    XMLHttpRequest: XMLHttpRequest_, WebSocket: noNet("WebSocket"), EventSource: noNet("EventSource"),
    MutationObserver: Observer_, WebKitMutationObserver: Observer_, ResizeObserver: Observer_,
    PerformanceObserver: Observer_, IntersectionObserver: IntersectionObserver_,
    customElements: { define: function () {}, get: function () { return undefined; }, whenDefined: function () { return new Promise(function () {}); }, upgrade: function () {} },
    URL: URL_, webkitURL: URL_, URLSearchParams: URLSearchParams,
    TextEncoder: TextEncoder_, TextDecoder: TextDecoder_,
    atob: atob_, btoa: btoa_,
    Node: Node, Element: Element, HTMLElement: HTMLElement, SVGElement: SVGElement, Text: Text,
    Comment: Comment, CharacterData: CharacterData, DocumentFragment: DocumentFragment,
    Document: Document, HTMLDocument: HTMLDocument, EventTarget: EventTarget_, NodeFilter: NodeFilter,
    Event: Event, CustomEvent: CustomEvent, UIEvent: UIEvent, MouseEvent: MouseEvent,
    KeyboardEvent: KeyboardEvent, FocusEvent: FocusEvent, InputEvent: InputEvent,
    PointerEvent: PointerEvent, WheelEvent: WheelEvent, TouchEvent: TouchEvent,
    MessageEvent: MessageEvent, ProgressEvent: ProgressEvent, ErrorEvent: ErrorEvent,
    PopStateEvent: PopStateEvent, HashChangeEvent: HashChangeEvent, StorageEvent: StorageEvent,
    AnimationEvent: AnimationEvent, TransitionEvent: TransitionEvent,
    Range: Range_, Location: Location, DOMParser: class {
      parseFromString(s) { var d = newDocument(fmtURL(PAGE)); parseHTML(d, String(s), null, false); return d; }
    },
    XMLSerializer: class { serializeToString(n) { var o = []; ser(n, o, false); return o.join(""); } },
    Image: function (w, h) { var el = DOC.createElement("img"); if (w) el.width = w; if (h) el.height = h; return el; },
    Option: function (text, value, def, sel) {
      var el = DOC.createElement("option");
      if (text !== undefined) el.textContent = text;
      if (value !== undefined) el.setAttribute("value", value);
      if (sel) el.setAttribute("selected", "");
      return el;
    },
    Audio: function () { return DOC.createElement("audio"); },
    CSS: { supports: function () { return false; }, escape: function (s) { return String(s).replace(/([^\w-])/g, "\\$1"); } },
    crypto: {
      getRandomValues: function (a) { for (var i = 0; i < a.length; i++) a[i] = Math.floor(Math.random() * 4294967296); return a; },
      randomUUID: function () { return "xxxxxxxx-xxxx-4xxx-yxxx-xxxxxxxxxxxx".replace(/[xy]/g, function (c) { var r = Math.random() * 16 | 0; return (c === "x" ? r : (r & 3 | 8)).toString(16); }); },
      subtle: undefined
    },
    structuredClone: function (v) { return v === undefined ? v : JSON.parse(JSON.stringify(v)); },
    Blob: class { constructor(parts, o) { this._s = (parts || []).join(""); this.size = this._s.length; this.type = (o && o.type) || ""; } text() { return Promise.resolve(this._s); } slice() { return this; } },
    FormData: class { constructor() { this._l = []; } append(k, v) { this._l.push([k, v]); } get(k) { for (var i = 0; i < this._l.length; i++) if (this._l[i][0] === k) return this._l[i][1]; return null; } has(k) { return this.get(k) !== null; } set(k, v) { this.append(k, v); } delete() {} entries() { return this._l[Symbol.iterator](); } },
    Headers: class { constructor() { this._h = {}; } get(k) { return this._h[String(k).toLowerCase()] || null; } set(k, v) { this._h[String(k).toLowerCase()] = String(v); } append(k, v) { this.set(k, v); } has(k) { return String(k).toLowerCase() in this._h; } },
    AbortController: class { constructor() { this.signal = { aborted: false, addEventListener: function () {}, removeEventListener: function () {} }; } abort() { this.signal.aborted = true; } },
    BroadcastChannel: class { postMessage() {} close() {} addEventListener() {} },
    Worker: class { constructor(u) { log("Worker nao suportado: " + short(u)); } postMessage() {} terminate() {} addEventListener() {} },
    Notification: { permission: "denied", requestPermission: function () { return Promise.resolve("denied"); } },
    visualViewport: { width: INNER_W, height: INNER_H, scale: 1, offsetLeft: 0, offsetTop: 0, addEventListener: function () {} },
    onload: null, onerror: null, onresize: null, onscroll: null, onpopstate: null, onhashchange: null,
    onmessage: null, onbeforeunload: null, onunload: null, onpageshow: null, onfocus: null, onblur: null,
    addEventListener: function (t, fn, o) { addEL(G, t, fn, o); },
    removeEventListener: function (t, fn) { removeEL(G, t, fn); },
    dispatchEvent: function (e) { return dispatch(G, e); }
  };
  Object.keys(CLASSES).forEach(function (k) { ALL[k] = CLASSES[k]; });
  Object.keys(ALL).forEach(function (k) {
    try { Object.defineProperty(G, k, { value: ALL[k], writable: true, configurable: true, enumerable: false }); }
    catch (e) { try { G[k] = ALL[k]; } catch (e2) {} }
  });
  Object.defineProperty(G, "location", { configurable: true, get: function () { return LOC; }, set: function (v) { go(v); } });
  Object.defineProperty(G, "event", { configurable: true, writable: true, value: undefined });
  /* NodeList.item() e afins: as listas daqui sao arrays */
  if (!Array.prototype.item) Object.defineProperty(Array.prototype, "item", { configurable: true, writable: true, value: function (i) { var v = this[i]; return v === undefined ? null : v; } });
  if (!Array.prototype.namedItem) Object.defineProperty(Array.prototype, "namedItem", { configurable: true, writable: true, value: function (n) { for (var i = 0; i < this.length; i++) if (this[i] && (this[i].id === n || this[i].name === n)) return this[i]; return null; } });
  /* o que o shell do SpiderMonkey poe no global e um navegador nao tem */
  ["os", "scriptArgs", "scriptPath", "read", "snarf", "readline", "readlineBuf", "putstr", "printErr",
   "quit", "load", "evaluate", "redirect", "timeout", "sleep", "options", "version", "gc"].forEach(function (k) {
    try { delete G[k]; } catch (e) {}
  });

  /* ---------- carga ---------- */
  var html = String(DADOS.html || "");
  DADOS.html = null;
  mark("DADOS.JS lido, " + html.length + " chars");
  parseHTML(DOC, html, null, false);
  var baseEl = DOC.querySelector("base[href]");
  DOC._base = baseEl ? resolve(baseEl.getAttribute("href"), PAGE_URL) : PAGE_URL;
  var tParse = dateNow_() - T0;
  mark("HTML analisado");

  /* laco infinito numa pagina: o shell interrompe e a foto sai como estiver */
  var emitted = false;
  if (typeof timeout_ === "function") {
    try {
      timeout_(HARD_S, function () {
        putstr_("[js] tempo esgotado (" + HARD_S + " s)\n");
        try { emit("tempo esgotado (" + HARD_S + " s): a pagina ficou presa num laco"); }
        catch (e) { putstr_("[js] a foto falhou: " + e + "\n"); }
        return false;
      });
    } catch (e) {}
  }

  PHASE = 1;
  /* scripts do HTML, na ordem (os que document.write cria rodam dentro do
   * runScript de quem escreveu) */
  var mains = [];
  walkEl(DOC, function (e) { if (e._n === "script" && e._main) mains.push(e); return false; });
  mark(mains.length + " scripts no HTML");
  for (var si = 0; si < mains.length; si++) {
    if (dateNow_() - T0 > BUDGET_MS) { log("tempo esgotado nos scripts"); break; }
    if (!mains[si]._done && mains[si].isConnected) runScript(mains[si]);
  }
  mark("scripts rodados");
  DOC._rs = "interactive";
  dispatch(DOC, new Event("readystatechange"));
  dispatch(DOC, new Event("DOMContentLoaded", { bubbles: true }));
  DOC._rs = "complete";
  dispatch(DOC, new Event("readystatechange"));
  var loadEv = new Event("load");
  loadEv.target = DOC;
  fire(G, loadEv);
  drain();
  var bodyOnload = DOC.body && DOC.body.getAttribute("onload");
  if (bodyOnload && typeof G.onload !== "function") runCode(bodyOnload, "body onload");
  dispatch(G, new Event("pageshow"));
  mark("DOMContentLoaded e load");
  PHASE = 2;
  runTimers();
  drain();
  mark(nTimers + " timers");

  if (DEBUG) G.__BR = { DOC: DOC, ser: ser, walk: walk };
  if (!G.__BRNOEMIT) emit(null);

  /* ---------- saida ---------- */
  function emit(why) {
    if (emitted) return;
    emitted = true;
    if (why) log(why);
    var out = [];
    ser(DOC, out, true);
    var res = out.join("");
    mark("serializado, " + res.length + " chars");
    LOG.push("[js] parse " + tParse + " ms, scripts e timers " + (dateNow_() - T0 - tParse) + " ms, " +
      NSCRIPT + " inline, " + NEXT + " externos, " + nTimers + " timers");
    if (IR) LOG.push("[js-ir] " + IR);
    for (var fi = 0; fi < FALTA.length && fi < 20; fi++) LOG.push("[js-falta] " + FALTA[fi]);
    putstr_(LOG.join("\n") + "\n<<<BRDOM>>>\n");
    putstr_(res);
    putstr_("\n");
  }
})(this);
