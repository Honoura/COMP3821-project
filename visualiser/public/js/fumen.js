// Fumen (v115, and read-only v110) encoder/decoder.
// Format reference: knewjade/tetris-fumen and perfect-tetris' FumenReader.
(function (root) {
  const Tet = typeof module !== 'undefined' && module.exports ? require('./tetris.js') : root.Tet;
  const { Field, pieceCells, WIDTH } = Tet;

  const B64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const COMMENT_TABLE =
    ' !"#$%&\'()*+,-./0123456789:;<=>?@ABCDEFGHIJKLMNOPQRSTUVWXYZ[\\]^_`abcdefghijklmnopqrstuvwxyz{|}~';
  // Fumen stores rotation as 0 reverse, 1 right, 2 spawn, 3 left; this map
  // converts both ways between that and 0 spawn, 1 right, 2 reverse, 3 left.
  const ROT_MAP = [2, 1, 0, 3];

  // Fumen anchors O, I, S and Z differently from their rotation centre.
  function legacyShift(type, rot) {
    if (type === 3) return [[0, -1], [0, 0], [1, 0], [1, -1]][rot]; // O
    if (type === 1) return [[0, 0], [0, 0], [1, 0], [0, -1]][rot]; // I
    if (type === 7) return [[0, -1], [-1, 0], [0, 0], [0, 0]][rot]; // S
    if (type === 4) return [[0, -1], [0, 0], [0, 0], [1, 0]][rot]; // Z
    return [0, 0];
  }

  function parseQuiz(text) {
    const m = /^#Q=\[([IOTLJSZ]?)\]\(([IOTLJSZ]?)\)([IOTLJSZ]*)/.exec(text);
    if (!m) return null;
    const id = (c) => (c ? Tet.pieceId(c) : 0);
    return { hold: id(m[1]), current: id(m[2]), next: [...m[3]].map(id) };
  }

  function decode(input) {
    const match = /([vmdVMD])(115|110)@([A-Za-z0-9+/?]+)/.exec(String(input).trim());
    if (!match) throw new Error('Not a fumen string (expected something like "v115@...")');
    const fieldTop = match[2] === '115' ? 23 : 21;
    const blocks = (fieldTop + 1) * WIDTH;
    const data = match[3].replace(/\?/g, '');
    let pos = 0;
    const poll = (n) => {
      let v = 0;
      for (let i = 0, mul = 1; i < n; i++, mul *= 64) {
        if (pos >= data.length) throw new Error('Fumen data ended unexpectedly');
        const c = B64.indexOf(data[pos++]);
        if (c < 0) throw new Error('Invalid character in fumen data');
        v += c * mul;
      }
      return v;
    };

    const pages = [];
    let prev = new Field(fieldTop);
    let repeat = 0;
    let comment = '';
    let quiz = null;
    while (pos < data.length) {
      // Field, stored as a run-length encoded diff against the previous page.
      const field = prev.copy();
      if (repeat > 0) {
        repeat--;
      } else {
        let i = 0;
        let unchanged = false;
        while (i < blocks) {
          const run = poll(2);
          const diff = Math.floor(run / blocks) - 8;
          const len = (run % blocks) + 1;
          if (diff === 0 && len === blocks) unchanged = true;
          for (let k = 0; k < len && i < blocks; k++, i++) {
            const x = i % WIDTH;
            const y = fieldTop - Math.floor(i / WIDTH) - 1;
            if (diff) field.set(x, y, field.get(x, y) + diff);
          }
        }
        if (unchanged) repeat = poll(1);
      }

      // Piece and flags.
      let v = poll(3);
      const type = v % 8; v = Math.floor(v / 8);
      const rot = ROT_MAP[v % 4]; v = Math.floor(v / 4);
      const loc = v % blocks; v = Math.floor(v / blocks);
      const rise = v % 2 === 1; v = Math.floor(v / 2);
      const mirror = v % 2 === 1; v = Math.floor(v / 2);
      v = Math.floor(v / 2); // colour flag (only meaningful on the first page)
      const hasComment = v % 2 === 1; v = Math.floor(v / 2);
      const lock = v % 2 === 0;

      let piece = null;
      if (type > 0 && type < 8) {
        const [sx, sy] = legacyShift(type, rot);
        piece = { type, rot, x: (loc % WIDTH) + sx, y: fieldTop - Math.floor(loc / WIDTH) - 1 + sy };
      }

      if (hasComment) {
        const len = poll(2);
        let raw = '';
        for (let i = 0; i < len; i += 4) {
          let c = poll(5);
          for (let k = 0; k < 4 && i + k < len; k++) {
            raw += COMMENT_TABLE[c % 96] || '';
            c = Math.floor(c / 96);
          }
        }
        try { comment = unescape(raw); } catch (e) { comment = raw; }
        quiz = parseQuiz(comment);
      }

      const page = {
        field,
        piece,
        lock,
        rise,
        mirror,
        comment: quiz ? '' : comment,
        quiz: quiz ? { hold: quiz.hold, current: quiz.current, next: quiz.next.slice() } : null,
      };
      pages.push(page);

      // Apply the page's operation to get the next page's starting field.
      const after = field.copy();
      if (lock) {
        if (piece) {
          after.put(piece);
          if (quiz) quiz = advanceQuiz(quiz, piece.type);
        }
        after.clearLines();
        if (rise) after.rise();
        if (mirror) after.mirror();
      }
      prev = after;
    }
    return pages;
  }

  // Quiz semantics: if the placed piece is not the current one, swap with hold.
  function advanceQuiz(q, type) {
    q = { hold: q.hold, current: q.current || q.next.shift() || 0, next: q.next };
    if (type !== q.current) {
      [q.hold, q.current] = [q.current, q.hold];
      if (!q.current) q.current = q.next.shift() || 0;
    }
    if (type !== q.current) return null; // sequence no longer matches: stop tracking
    q.current = q.next.shift() || 0;
    return q;
  }

  // pages: [{ field: Field (before the piece locks), piece?: {type, rot, x, y},
  //           lock?: bool (default true), comment?: string (omit to keep the previous one) }]
  function encode(pages) {
    const fieldTop = 23;
    const blocks = (fieldTop + 1) * WIDTH;
    const out = [];
    const push = (v, n) => {
      for (let i = 0; i < n; i++) { out.push(B64[v % 64]); v = Math.floor(v / 64); }
    };

    let prev = new Field(fieldTop);
    let repeatAt = -1; // index of the open "unchanged pages" counter, if any
    let prevComment = '';
    pages.forEach((page, index) => {
      const field = page.field || prev;
      const diffs = [];
      for (let i = 0; i < blocks; i++) {
        const x = i % WIDTH;
        const y = fieldTop - Math.floor(i / WIDTH) - 1;
        diffs.push(field.get(x, y) - prev.get(x, y) + 8);
      }
      if (diffs.every((d) => d === 8)) {
        if (repeatAt >= 0 && B64.indexOf(out[repeatAt]) < 63) {
          out[repeatAt] = B64[B64.indexOf(out[repeatAt]) + 1];
        } else {
          push(8 * blocks + blocks - 1, 2);
          repeatAt = out.length;
          push(0, 1);
        }
      } else {
        repeatAt = -1;
        for (let i = 0; i < blocks;) {
          let j = i;
          while (j + 1 < blocks && diffs[j + 1] === diffs[i]) j++;
          push(diffs[i] * blocks + (j - i), 2);
          i = j + 1;
        }
      }

      const piece = page.piece;
      const lock = page.lock !== false;
      const comment = page.comment === undefined ? prevComment : page.comment;
      const hasComment = index === 0 ? comment !== '' : comment !== prevComment;
      let loc = 0;
      let type = 0;
      let rot = 0;
      if (piece) {
        const [sx, sy] = legacyShift(piece.type, piece.rot);
        type = piece.type;
        rot = piece.rot;
        loc = (fieldTop - (piece.y - sy) - 1) * WIDTH + (piece.x - sx);
      }
      let v = lock ? 0 : 1;
      v = v * 2 + (hasComment ? 1 : 0);
      v = v * 2 + (index === 0 ? 1 : 0); // colour
      v = v * 2; // mirror
      v = v * 2; // rise
      v = v * blocks + loc;
      v = v * 4 + ROT_MAP[rot];
      v = v * 8 + type;
      push(v, 3);

      if (hasComment) {
        const esc = escape(comment).slice(0, 4095);
        push(esc.length, 2);
        for (let i = 0; i < esc.length; i += 4) {
          let c = 0;
          for (let k = 3; k >= 0; k--) c = c * 96 + Math.max(0, COMMENT_TABLE.indexOf(esc[i + k] || ' '));
          push(c, 5);
        }
        prevComment = comment;
      }

      const after = field.copy();
      if (lock && piece) {
        after.put(piece);
        after.clearLines();
      } else if (lock) {
        after.clearLines();
      }
      prev = after;
    });
    return 'v115@' + out.join('');
  }

  function quizComment(hold, current, next) {
    const L = Tet.LETTERS;
    return `#Q=[${hold ? L[hold] : ''}](${current ? L[current] : ''})${next.map((t) => L[t]).join('')}`;
  }

  const api = { decode, encode, quizComment, parseQuiz };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Fumen = api;
})(typeof window !== 'undefined' ? window : globalThis);
