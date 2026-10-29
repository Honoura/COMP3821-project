// Canvas drawing for the board, hold/next previews and the timeline.
(function (root) {
  const { pieceCells, LETTERS } = root.Tet;

  const css = getComputedStyle(document.documentElement);
  const COLORS = {};
  for (let t = 1; t <= 8; t++) COLORS[t] = css.getPropertyValue(`--piece-${LETTERS[t]}`).trim();

  // fluid: the canvas width comes from CSS, so don't pin it inline.
  function setupCanvas(canvas, w, h, fluid = false) {
    const dpr = window.devicePixelRatio || 1;
    if (canvas.width !== Math.round(w * dpr) || canvas.height !== Math.round(h * dpr)) {
      canvas.width = Math.round(w * dpr);
      canvas.height = Math.round(h * dpr);
    }
    if (!fluid) canvas.style.width = w + 'px';
    canvas.style.height = h + 'px';
    const ctx = canvas.getContext('2d');
    ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
    ctx.clearRect(0, 0, w, h);
    return ctx;
  }

  function roundRect(ctx, x, y, w, h, r) {
    ctx.beginPath();
    ctx.moveTo(x + r, y);
    ctx.arcTo(x + w, y, x + w, y + h, r);
    ctx.arcTo(x + w, y + h, x, y + h, r);
    ctx.arcTo(x, y + h, x, y, r);
    ctx.arcTo(x, y, x + w, y, r);
    ctx.closePath();
  }

  function shade(hex, amt) {
    const n = parseInt(hex.slice(1), 16);
    const f = (c) => Math.max(0, Math.min(255, Math.round(c + (amt > 0 ? (255 - c) * amt : c * amt))));
    return `rgb(${f(n >> 16)},${f((n >> 8) & 255)},${f(n & 255)})`;
  }

  // One mino: base colour with a soft top highlight and darker rim.
  function drawCell(ctx, x, y, s, type, { alpha = 1, glow = false } = {}) {
    const base = COLORS[type] || COLORS[8];
    const g = Math.max(1, s * 0.06);
    ctx.save();
    ctx.globalAlpha = alpha;
    if (glow) {
      ctx.shadowColor = base;
      ctx.shadowBlur = s * 0.7;
    }
    const grad = ctx.createLinearGradient(0, y, 0, y + s);
    grad.addColorStop(0, shade(base, 0.18));
    grad.addColorStop(1, shade(base, -0.12));
    ctx.fillStyle = grad;
    roundRect(ctx, x + g, y + g, s - 2 * g, s - 2 * g, s * 0.16);
    ctx.fill();
    ctx.shadowBlur = 0;
    ctx.strokeStyle = shade(base, -0.35);
    ctx.lineWidth = 1;
    ctx.stroke();
    ctx.fillStyle = 'rgba(255,255,255,0.22)';
    roundRect(ctx, x + g * 2.2, y + g * 2.2, s - 4.4 * g, s * 0.22, s * 0.1);
    ctx.fill();
    ctx.restore();
  }

  // frame: see frames.js. opts: { rows, cell, dropT (0..1 entry animation), pulse }
  function drawBoard(canvas, frame, opts) {
    const rows = opts.rows;
    const s = opts.cell;
    const W = 10 * s;
    const H = rows * s;
    const ctx = setupCanvas(canvas, W, H);

    ctx.fillStyle = '#0a0c11';
    ctx.fillRect(0, 0, W, H);
    // Column shading by colour class (parity lemmas): even columns slightly lighter.
    for (let x = 0; x < 10; x += 2) {
      ctx.fillStyle = 'rgba(255,255,255,0.018)';
      ctx.fillRect(x * s, 0, s, H);
    }
    ctx.fillStyle = 'rgba(255,255,255,0.07)';
    for (let x = 1; x < 10; x++)
      for (let y = 1; y < rows; y++) ctx.fillRect(x * s - 1, y * s - 1, 2, 2);

    // PC height guides.
    ctx.font = `600 ${Math.max(9, s * 0.3)}px Inter, sans-serif`;
    ctx.textAlign = 'right';
    for (const h of [2, 4, 6]) {
      if (h >= rows) continue;
      const py = H - h * s;
      ctx.strokeStyle = h === 4 ? 'rgba(124,140,255,0.28)' : 'rgba(124,140,255,0.14)';
      ctx.setLineDash([4, 5]);
      ctx.beginPath();
      ctx.moveTo(0, py + 0.5);
      ctx.lineTo(W, py + 0.5);
      ctx.stroke();
      ctx.setLineDash([]);
      ctx.fillStyle = 'rgba(160,170,255,0.45)';
      ctx.fillText(String(h), W - 4, py - 3);
    }
    if (!frame) return;

    const toPx = (x, y) => [x * s, H - (y + 1) * s];
    const f = frame.field;
    for (let y = 0; y < rows; y++)
      for (let x = 0; x < 10; x++) {
        const t = f.get(x, y);
        if (t) drawCell(ctx, ...toPx(x, y), s, t);
      }

    if (frame.piece) {
      const p = frame.piece;
      const ease = 1 - Math.pow(1 - (opts.dropT ?? 1), 3);
      const lift = (1 - ease) * 2.5;
      const cells = pieceCells(p.type, p.rot, p.x, p.y);
      ctx.save();
      for (const [x, y] of cells) {
        const [px, py] = toPx(x, y);
        ctx.strokeStyle = 'rgba(255,255,255,0.18)';
        ctx.lineWidth = 1;
        roundRect(ctx, px + 2.5, py + 2.5, s - 5, s - 5, s * 0.16);
        ctx.stroke();
      }
      ctx.restore();
      for (const [x, y] of cells) {
        const [px, py] = toPx(x, y + lift);
        drawCell(ctx, px, py, s, p.type, { glow: true, alpha: frame.lock === false ? 0.55 : 0.4 + 0.6 * ease });
        ctx.strokeStyle = `rgba(255,255,255,${0.75 * ease})`;
        ctx.lineWidth = 1.5;
        roundRect(ctx, px + 1.5, py + 1.5, s - 3, s - 3, s * 0.18);
        ctx.stroke();
      }
    }

    // Rows this piece completes.
    if (frame.clearRows && frame.clearRows.length) {
      for (const y of frame.clearRows) {
        if (y >= rows) continue;
        const py = H - (y + 1) * s;
        const grad = ctx.createLinearGradient(0, 0, W, 0);
        const a = 0.12 + 0.1 * (opts.pulse ?? 0);
        grad.addColorStop(0, `rgba(255,255,255,${a * 0.4})`);
        grad.addColorStop(0.5, `rgba(255,255,255,${a})`);
        grad.addColorStop(1, `rgba(255,255,255,${a * 0.4})`);
        ctx.fillStyle = grad;
        ctx.fillRect(0, py, W, s);
      }
    }
  }

  function drawMiniPiece(ctx, type, cx, cy, s, alpha = 1) {
    const cells = pieceCells(type, 0, 0, 0);
    const xs = cells.map((c) => c[0]);
    const ys = cells.map((c) => c[1]);
    const minX = Math.min(...xs), maxX = Math.max(...xs), minY = Math.min(...ys), maxY = Math.max(...ys);
    const ox = cx - ((maxX - minX + 1) * s) / 2;
    const oy = cy - ((maxY - minY + 1) * s) / 2;
    for (const [x, y] of cells) drawCell(ctx, ox + (x - minX) * s, oy + (maxY - y) * s, s, type, { alpha });
  }

  function drawHold(canvas, type, dim) {
    const horizontal = window.matchMedia('(max-width: 560px)').matches;
    const ctx = horizontal ? setupCanvas(canvas, 72, 56) : setupCanvas(canvas, 96, 72);
    if (type) drawMiniPiece(ctx, type, horizontal ? 36 : 48, horizontal ? 28 : 36, horizontal ? 13 : 18, dim ? 0.45 : 1);
  }

  // Vertical column beside the board; a horizontal strip on narrow screens.
  function drawNext(canvas, next) {
    const horizontal = window.matchMedia('(max-width: 560px)').matches;
    const ctx = horizontal ? setupCanvas(canvas, 260, 56) : setupCanvas(canvas, 96, 300);
    next.slice(0, 5).forEach((t, i) => {
      const s = horizontal ? (i === 0 ? 11 : 9) : i === 0 ? 18 : 15;
      if (horizontal) drawMiniPiece(ctx, t, 28 + i * 51, 28, s, i === 0 ? 1 : 0.85);
      else drawMiniPiece(ctx, t, 48, 34 + i * 58, s, i === 0 ? 1 : 0.85);
    });
  }

  // markers: [{ index, kind: 'l2' | 'l4' | 'other' | 'error' }]
  function drawTimeline(canvas, count, index, markers, hover) {
    const w = canvas.clientWidth;
    const h = 44;
    const ctx = setupCanvas(canvas, w, h, true);
    const pad = 10;
    const track = w - 2 * pad;
    const xAt = (i) => pad + (count <= 1 ? 0 : (i / (count - 1)) * track);
    const ty = 26;

    ctx.fillStyle = '#1b2030';
    roundRect(ctx, pad, ty - 3, track, 6, 3);
    ctx.fill();
    if (count > 0) {
      const grad = ctx.createLinearGradient(pad, 0, pad + track, 0);
      grad.addColorStop(0, '#7c8cff');
      grad.addColorStop(1, '#a78bfa');
      ctx.fillStyle = grad;
      roundRect(ctx, pad, ty - 3, Math.max(6, xAt(index) - pad), 6, 3);
      ctx.fill();
    }
    const kinds = { l2: COLORS[1], l4: COLORS[5], other: '#9aa3b5', error: '#f87171' };
    for (const m of markers) {
      const x = xAt(m.index);
      ctx.fillStyle = kinds[m.kind];
      ctx.globalAlpha = m.index <= index ? 1 : 0.6;
      roundRect(ctx, x - 1.5, 6, 3, m.kind === 'error' ? 30 : 12, 1.5);
      ctx.fill();
      ctx.globalAlpha = 1;
    }
    if (hover != null && count > 0) {
      ctx.fillStyle = 'rgba(255,255,255,0.25)';
      ctx.fillRect(xAt(hover) - 0.5, ty - 8, 1, 16);
    }
    if (count > 0) {
      const x = xAt(index);
      ctx.fillStyle = '#fff';
      ctx.shadowColor = 'rgba(124,140,255,0.9)';
      ctx.shadowBlur = 10;
      ctx.beginPath();
      ctx.arc(x, ty, 6.5, 0, Math.PI * 2);
      ctx.fill();
      ctx.shadowBlur = 0;
    }
    return { indexAt: (px) => Math.round(Math.max(0, Math.min(1, (px - pad) / track)) * (count - 1)) };
  }

  root.Render = { drawBoard, drawHold, drawNext, drawTimeline, COLORS };
})(window);
