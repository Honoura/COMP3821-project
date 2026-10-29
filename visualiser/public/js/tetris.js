// Piece shapes and a minimal playfield, shared by the fumen codec and the
// game replayer. Piece ids follow fumen: 0 empty, 1 I, 2 L, 3 O, 4 Z, 5 T,
// 6 J, 7 S, 8 garbage. Rotations follow tetris.h: 0 spawn, 1 right,
// 2 reverse, 3 left. Coordinates are (x, y) with y = 0 the bottom row.
(function (root) {
  const LETTERS = ['', 'I', 'L', 'O', 'Z', 'T', 'J', 'S', 'X'];
  const WIDTH = 10;

  const BASE = {
    1: [[0, 0], [-1, 0], [1, 0], [2, 0]],
    2: [[0, 0], [-1, 0], [1, 0], [1, 1]],
    3: [[0, 0], [1, 0], [0, 1], [1, 1]],
    4: [[0, 0], [1, 0], [0, 1], [-1, 1]],
    5: [[0, 0], [-1, 0], [1, 0], [0, 1]],
    6: [[0, 0], [-1, 0], [1, 0], [-1, 1]],
    7: [[0, 0], [-1, 0], [0, 1], [1, 1]],
  };

  function pieceId(letter) {
    const i = LETTERS.indexOf(String(letter).toUpperCase());
    return i > 0 && i < 8 ? i : 0;
  }

  // Cells covered by a piece with rotation centre (x, y).
  function pieceCells(type, rot, x, y) {
    return BASE[type].map(([dx, dy]) => {
      for (let r = 0; r < (rot & 3); r++) [dx, dy] = [dy, -dx];
      return [x + dx, y + dy];
    });
  }

  class Field {
    constructor(height = 23) {
      this.height = height;
      this.cells = new Uint8Array(height * WIDTH);
      this.garbage = new Uint8Array(WIDTH);
    }
    copy() {
      const f = new Field(this.height);
      f.cells.set(this.cells);
      f.garbage.set(this.garbage);
      return f;
    }
    get(x, y) {
      if (y === -1) return this.garbage[x];
      if (x < 0 || x >= WIDTH || y < 0 || y >= this.height) return 0;
      return this.cells[y * WIDTH + x];
    }
    set(x, y, v) {
      if (y === -1) this.garbage[x] = v;
      else if (x >= 0 && x < WIDTH && y >= 0 && y < this.height) this.cells[y * WIDTH + x] = v;
    }
    canPlace(piece) {
      return pieceCells(piece.type, piece.rot, piece.x, piece.y).every(
        ([x, y]) => x >= 0 && x < WIDTH && y >= 0 && y < this.height && !this.get(x, y));
    }
    put(piece) {
      for (const [x, y] of pieceCells(piece.type, piece.rot, piece.x, piece.y)) this.set(x, y, piece.type);
    }
    fullRows() {
      const rows = [];
      for (let y = 0; y < this.height; y++) {
        let full = true;
        for (let x = 0; x < WIDTH && full; x++) full = this.cells[y * WIDTH + x] !== 0;
        if (full) rows.push(y);
      }
      return rows;
    }
    clearLines() {
      const full = new Set(this.fullRows());
      if (!full.size) return 0;
      const out = new Uint8Array(this.cells.length);
      let w = 0;
      for (let y = 0; y < this.height; y++) {
        if (full.has(y)) continue;
        out.set(this.cells.subarray(y * WIDTH, (y + 1) * WIDTH), w * WIDTH);
        w++;
      }
      this.cells = out;
      return full.size;
    }
    // Fumen "rise": push everything up a row, garbage row becomes the bottom row.
    rise() {
      const out = new Uint8Array(this.cells.length);
      out.set(this.garbage, 0);
      out.set(this.cells.subarray(0, (this.height - 1) * WIDTH), WIDTH);
      this.cells = out;
      this.garbage = new Uint8Array(WIDTH);
    }
    mirror() {
      for (let y = 0; y < this.height; y++) this.cells.subarray(y * WIDTH, (y + 1) * WIDTH).reverse();
    }
    isEmpty() {
      return this.cells.every((v) => v === 0);
    }
    stackHeight() {
      for (let y = this.height - 1; y >= 0; y--)
        for (let x = 0; x < WIDTH; x++) if (this.cells[y * WIDTH + x]) return y + 1;
      return 0;
    }
    equals(o) {
      return this.cells.every((v, i) => v === o.cells[i]) && this.garbage.every((v, i) => v === o.garbage[i]);
    }
  }

  const api = { LETTERS, WIDTH, pieceId, pieceCells, Field };
  if (typeof module !== 'undefined' && module.exports) module.exports = api;
  else root.Tet = api;
})(typeof window !== 'undefined' ? window : globalThis);
