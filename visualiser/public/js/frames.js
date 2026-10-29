// Turns a runner game log or decoded fumen pages into a list of frames for
// the renderer. A frame is the field before a piece locks plus that piece:
//   { field, piece, hold, next, comment, clearRows, isPC, pcIndex, pcTotal,
//     usedHold, label, error }
(function (root) {
  const isNode = typeof module !== 'undefined' && module.exports;
  const Tet = isNode ? require('./tetris.js') : root.Tet;
  const Fumen = isNode ? require('./fumen.js') : root.Fumen;
  const { Field, pieceId } = Tet;

  const NEXT_SHOWN = 5;

  function withPlacement(field, piece) {
    const f = field.copy();
    if (piece) f.put(piece);
    return f;
  }

  function fromGame(game) {
    const queue = [...game.queue].map(pieceId);
    const frames = [];
    let field = new Field(23);
    let current = queue[0] || 0;
    let next = 1;
    let hold = 0;
    let pcs = 0;
    game.moves.forEach((m, k) => {
      if (m.hold) {
        if (!hold) { hold = current; current = queue[next++] || 0; }
        else [hold, current] = [current, hold];
      }
      const piece = { type: pieceId(m.piece), rot: m.rot, x: m.x, y: m.y };
      const clearRows = withPlacement(field, piece).fullRows();
      if (m.pc) pcs++;
      frames.push({
        field,
        piece,
        hold,
        next: queue.slice(next, next + NEXT_SHOWN),
        clearRows,
        isPC: m.pc,
        pcTotal: pcs,
        usedHold: m.hold,
        move: k,
        label: `Piece ${k + 1} / ${queue.length}`,
      });
      field = withPlacement(field, piece);
      field.clearLines();
      current = queue[next++] || 0;
    });

    // Final frame: the resulting board, plus the rejected move if any.
    const last = { field, piece: null, hold, next: queue.slice(next - 1, next - 1 + NEXT_SHOWN), clearRows: [],
      isPC: false, pcTotal: pcs, usedHold: false, move: game.moves.length, label: 'End' };
    if (game.error) last.error = game.error.message;
    frames.push(last);
    return frames;
  }

  function fromFumen(text) {
    const pages = Fumen.decode(text);
    let pcs = 0;
    return pages.map((p, k) => {
      const after = withPlacement(p.field, p.lock ? p.piece : null);
      const clearRows = p.lock && p.piece ? after.fullRows() : [];
      after.clearLines();
      const isPC = !!(p.lock && p.piece && after.isEmpty());
      if (isPC) pcs++;
      const q = p.quiz;
      return {
        field: p.field,
        piece: p.piece,
        lock: p.lock,
        hold: q ? q.hold : 0,
        next: q ? q.next.slice(0, NEXT_SHOWN) : [],
        comment: p.comment,
        clearRows,
        isPC,
        pcTotal: pcs,
        usedHold: !!(q && p.piece && q.current && q.current !== p.piece.type),
        move: k,
        label: `Page ${k + 1} / ${pages.length}`,
      };
    });
  }

  // Exports a game as a fumen: one page per piece, with the whole queue in a
  // quiz comment on the first page so fumen viewers can track hold and next.
  function toFumen(game) {
    const queue = [...game.queue].map(pieceId);
    const frames = fromGame(game).filter((f) => f.piece);
    if (!frames.length) return Fumen.encode([{ field: new Field(23) }]);
    return Fumen.encode(frames.map((f, k) => ({
      field: f.field,
      piece: f.piece,
      comment: k === 0 ? Fumen.quizComment(0, queue[0], queue.slice(1)) : undefined,
    })));
  }

  const api = { fromGame, fromFumen, toFumen };
  if (isNode) module.exports = api;
  else root.Frames = api;
})(typeof window !== 'undefined' ? window : globalThis);
