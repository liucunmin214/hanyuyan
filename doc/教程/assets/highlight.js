/* 教程页脚本：汉语言代码块语法高亮（与 hanedit 编辑器同一词表与配色）+ 目录滚动高亮 */
(function () {
  'use strict';

  var KW = ['函数', '定义', '返回', '变量', '令', '常量', '如果', '若', '否则', '不然',
    '当', '循环', '只要', '对于', '遍历', '到', '结束', '完了', '打印', '输出', '显示',
    '真', '是', '假', '否', '空', '无', '与', '且', '或', '非', '跳出', '停止',
    '继续', '跳过', '导入', '引用', '从'];
  var TYPES = ['整数', '小数', '文字', '布尔', '列表', '字典'];

  function isIdentStart(c) {
    if (!c) return false;
    var code = c.charCodeAt(0);
    return (code >= 0x4E00 && code <= 0x9FFF) || (code >= 0x3400 && code <= 0x4DBF) ||
      code === 0x3007 || /[A-Za-z_]/.test(c);
  }
  function isIdentChar(c) {
    return isIdentStart(c) || (c >= '0' && c <= '9');
  }

  function esc(s) {
    return s.replace(/&/g, '&amp;').replace(/</g, '&lt;').replace(/>/g, '&gt;');
  }

  /* 与编辑器一致的扫描器：注释 > 字符串 > 数字 > 关键字/类型 */
  function highlight(src) {
    var out = '', i = 0, n = src.length;
    while (i < n) {
      var c = src[i], j;
      if (c === '#') {
        j = i;
        while (j < n && src[j] !== '\n') j++;
        out += '<span class="com">' + esc(src.slice(i, j)) + '</span>';
        i = j;
      } else if (c === '/' && src[i + 1] === '*') {
        var depth = 1;
        j = i + 2;
        while (j + 1 < n && depth > 0) {
          if (src[j] === '/' && src[j + 1] === '*') { depth++; j += 2; }
          else if (src[j] === '*' && src[j + 1] === '/') { depth--; j += 2; }
          else j++;
        }
        if (depth > 0) j = n;
        out += '<span class="com">' + esc(src.slice(i, j)) + '</span>';
        i = j;
      } else if (c === '"' || c === '“' || c === '「' || c === '『') {
        var close = c === '"' ? '"' : c === '“' ? '”' : c === '「' ? '」' : '』';
        j = i + 1;
        while (j < n) {
          if (src[j] === '\\') { j += 2; continue; }
          if (src[j] === close) { j++; break; }
          if (src[j] === '\n') break;
          j++;
        }
        out += '<span class="str">' + esc(src.slice(i, j)) + '</span>';
        i = j;
      } else if (c >= '0' && c <= '9') {
        j = i + 1;
        while (j < n && /[0-9A-Za-z._]/.test(src[j])) j++;
        out += '<span class="num">' + esc(src.slice(i, j)) + '</span>';
        i = j;
      } else if (isIdentStart(c)) {
        j = i + 1;
        while (j < n && isIdentChar(src[j])) j++;
        var word = src.slice(i, j);
        if (KW.indexOf(word) >= 0) out += '<span class="kw">' + esc(word) + '</span>';
        else if (TYPES.indexOf(word) >= 0) out += '<span class="type">' + esc(word) + '</span>';
        else out += esc(word);
        i = j;
      } else {
        out += esc(c);
        i++;
      }
    }
    return out;
  }

  document.addEventListener('DOMContentLoaded', function () {
    /* 代码块高亮 */
    var blocks = document.querySelectorAll('pre.code > code');
    for (var k = 0; k < blocks.length; k++) {
      blocks[k].innerHTML = highlight(blocks[k].textContent);
    }

    /* 目录滚动高亮 */
    var links = document.querySelectorAll('.toc a');
    var secs = [];
    for (var m = 0; m < links.length; m++) {
      var id = links[m].getAttribute('href');
      if (id && id.charAt(0) === '#') {
        var el = document.getElementById(id.slice(1));
        if (el) secs.push({ link: links[m], el: el });
      }
    }
    function spy() {
      var y = window.scrollY + 120, cur = null;
      for (var q = 0; q < secs.length; q++) {
        if (secs[q].el.offsetTop <= y) cur = secs[q];
      }
      for (var r = 0; r < secs.length; r++) secs[r].link.classList.remove('on');
      if (cur) cur.link.classList.add('on');
    }
    window.addEventListener('scroll', spy, { passive: true });
    spy();

    /* Mermaid 流水线图 */
    if (window.mermaid) {
      window.mermaid.initialize({ startOnLoad: true, theme: 'neutral', securityLevel: 'loose' });
    }
  });
})();
