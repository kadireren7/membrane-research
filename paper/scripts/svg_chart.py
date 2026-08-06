"""Tiny, dependency-free SVG chart helper for paper/scripts/generate-figures.py.

Deliberately not matplotlib/pandas: this environment has no network
access to install them (PEP 668 externally-managed, no venv set up),
and this project's own style favors minimal dependencies. Every chart
below is real, hand-generated SVG markup from real input data -- no
image library, no fabricated pixels.
"""
import html


def _fmt(x):
	return f"{x:.3f}".rstrip("0").rstrip(".") if isinstance(x, float) else str(x)


class SvgCanvas:
	def __init__(self, width=760, height=460, title=""):
		self.width = width
		self.height = height
		self.title = title
		self.elements = []

	def add(self, s):
		self.elements.append(s)

	def text(self, x, y, s, size=13, anchor="start", weight="normal", rotate=None, fill="#111"):
		t = f'transform="rotate({rotate} {x} {y})"' if rotate else ""
		self.add(
			f'<text x="{x}" y="{y}" font-size="{size}" font-family="Helvetica,Arial,sans-serif" '
			f'text-anchor="{anchor}" font-weight="{weight}" fill="{fill}" {t}>{html.escape(str(s))}</text>'
		)

	def line(self, x1, y1, x2, y2, stroke="#333", width=1, dash=None):
		d = f'stroke-dasharray="{dash}"' if dash else ""
		self.add(f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{stroke}" stroke-width="{width}" {d}/>')

	def rect(self, x, y, w, h, fill="#4C78A8", stroke="none"):
		self.add(f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}" stroke="{stroke}"/>')

	def circle(self, cx, cy, r, fill="#E45756"):
		self.add(f'<circle cx="{cx}" cy="{cy}" r="{r}" fill="{fill}"/>')

	def polyline(self, points, stroke="#4C78A8", width=2, fill="none"):
		pts = " ".join(f"{x},{y}" for x, y in points)
		self.add(f'<polyline points="{pts}" fill="{fill}" stroke="{stroke}" stroke-width="{width}"/>')

	def save(self, path):
		body = "\n".join(self.elements)
		svg = (
			f'<svg xmlns="http://www.w3.org/2000/svg" width="{self.width}" height="{self.height}" '
			f'viewBox="0 0 {self.width} {self.height}">\n'
			f'<rect x="0" y="0" width="{self.width}" height="{self.height}" fill="white"/>\n'
			f"{body}\n</svg>\n"
		)
		with open(path, "w") as f:
			f.write(svg)


PALETTE = ["#4C78A8", "#E45756", "#54A24B", "#EECA3B", "#B279A2", "#FF9DA6", "#9D755D"]


def grouped_bar_chart(path, title, groups, series_names, values, y_label, x_label="",
	margin_left=90, margin_bottom=90, width=820, height=480, log_scale=False, unit=""):
	"""groups: list of category labels (x-axis groups).
	series_names: list of series labels (one bar color per series).
	values: dict[series_name][group_index] -> float.
	"""
	import math

	c = SvgCanvas(width=width, height=height, title=title)
	c.text(width / 2, 28, title, size=16, anchor="middle", weight="bold")

	plot_left = margin_left
	plot_right = width - 30
	plot_top = 55
	plot_bottom = height - margin_bottom
	plot_w = plot_right - plot_left
	plot_h = plot_bottom - plot_top

	all_vals = [v for s in series_names for v in values[s] if v is not None]
	vmax = max(all_vals) if all_vals else 1.0
	vmin = 0.0
	if log_scale:
		vmin = min(v for v in all_vals if v > 0) * 0.5
		vmax = vmax * 2

	def y_of(v):
		if v is None:
			return plot_bottom
		if log_scale:
			lv = math.log10(max(v, vmin))
			lmin, lmax = math.log10(vmin), math.log10(vmax)
			return plot_bottom - (lv - lmin) / (lmax - lmin) * plot_h
		return plot_bottom - (v / vmax) * plot_h

	# axes
	c.line(plot_left, plot_top, plot_left, plot_bottom)
	c.line(plot_left, plot_bottom, plot_right, plot_bottom)
	# y gridlines/labels
	n_ticks = 5
	for i in range(n_ticks + 1):
		if log_scale:
			lmin, lmax = math.log10(vmin), math.log10(vmax)
			val = 10 ** (lmin + (lmax - lmin) * i / n_ticks)
		else:
			val = vmax * i / n_ticks
		yy = y_of(val)
		c.line(plot_left, yy, plot_right, yy, stroke="#ddd", width=1)
		c.text(plot_left - 8, yy + 4, f"{val:,.0f}" if val >= 1 else f"{val:.3g}", size=10, anchor="end")
	c.text(20, plot_top + plot_h / 2, y_label, size=12, anchor="middle", rotate=-90)

	group_w = plot_w / max(len(groups), 1)
	bar_w = group_w / (len(series_names) + 1)
	for gi, gname in enumerate(groups):
		gx0 = plot_left + gi * group_w
		for si, sname in enumerate(series_names):
			v = values[sname][gi]
			if v is None:
				continue
			bx = gx0 + (si + 0.5) * bar_w
			by = y_of(v)
			c.rect(bx, by, bar_w * 0.85, plot_bottom - by, fill=PALETTE[si % len(PALETTE)])
		c.text(gx0 + group_w / 2, plot_bottom + 18, gname, size=11, anchor="middle")

	if x_label:
		c.text(plot_left + plot_w / 2, height - 30, x_label, size=12, anchor="middle")

	legend_y = plot_top - 20
	lx = plot_left
	for si, sname in enumerate(series_names):
		c.rect(lx, legend_y - 10, 12, 12, fill=PALETTE[si % len(PALETTE)])
		c.text(lx + 16, legend_y, sname, size=11)
		lx += 16 + 9 * len(sname) + 20

	if unit:
		c.text(plot_right, height - 8, unit, size=10, anchor="end", fill="#666")
	c.save(path)


def line_chart(path, title, series, y_label, x_label, log_x=False, log_y=False,
	width=820, height=480, unit=""):
	"""series: dict[name] -> list of (x, y) tuples."""
	import math

	c = SvgCanvas(width=width, height=height, title=title)
	c.text(width / 2, 28, title, size=16, anchor="middle", weight="bold")

	plot_left, plot_right = 100, width - 30
	plot_top, plot_bottom = 55, height - 90
	plot_w, plot_h = plot_right - plot_left, plot_bottom - plot_top

	xs = [x for pts in series.values() for x, _ in pts]
	ys = [y for pts in series.values() for _, y in pts]
	xmin, xmax = min(xs), max(xs)
	ymin, ymax = 0, max(ys)
	if log_x:
		xmin = min(x for x in xs if x > 0)
	if log_y:
		ymin = min(y for y in ys if y > 0) * 0.5
		ymax = ymax * 2

	def x_of(x):
		if log_x:
			return plot_left + (math.log10(x) - math.log10(xmin)) / (math.log10(xmax) - math.log10(xmin)) * plot_w
		return plot_left + (x - xmin) / (xmax - xmin) * plot_w

	def y_of(y):
		if log_y:
			return plot_bottom - (math.log10(max(y, ymin)) - math.log10(ymin)) / (math.log10(ymax) - math.log10(ymin)) * plot_h
		return plot_bottom - (y - ymin) / (ymax - ymin) * plot_h

	c.line(plot_left, plot_top, plot_left, plot_bottom)
	c.line(plot_left, plot_bottom, plot_right, plot_bottom)
	for i in range(6):
		yy = plot_top + i * plot_h / 5
		c.line(plot_left, yy, plot_right, yy, stroke="#eee")

	for si, (name, pts) in enumerate(series.items()):
		pts_sorted = sorted(pts, key=lambda p: p[0])
		coords = [(x_of(x), y_of(y)) for x, y in pts_sorted]
		c.polyline(coords, stroke=PALETTE[si % len(PALETTE)], width=2.5)
		for x, y in coords:
			c.circle(x, y, 4, fill=PALETTE[si % len(PALETTE)])
		c.text(plot_left + 10, plot_top + 16 + si * 16, name, size=11, fill=PALETTE[si % len(PALETTE)])

	c.text(plot_left + plot_w / 2, height - 45, x_label, size=12, anchor="middle")
	c.text(25, plot_top + plot_h / 2, y_label, size=12, anchor="middle", rotate=-90)
	if unit:
		c.text(plot_right, height - 8, unit, size=10, anchor="end", fill="#666")
	c.save(path)


def scatter_chart(path, title, points, x_label, y_label, width=760, height=460, unit=""):
	"""points: list of (x, y, label, color_key)."""
	c = SvgCanvas(width=width, height=height, title=title)
	c.text(width / 2, 28, title, size=16, anchor="middle", weight="bold")
	plot_left, plot_right = 90, width - 30
	plot_top, plot_bottom = 55, height - 90
	plot_w, plot_h = plot_right - plot_left, plot_bottom - plot_top

	xs = [p[0] for p in points]
	ys = [p[1] for p in points]
	xmin, xmax = min(xs) * 0.95, max(xs) * 1.05
	ymin, ymax = min(ys) * 0.95, max(ys) * 1.05
	if xmax == xmin:
		xmax = xmin + 1
	if ymax == ymin:
		ymax = ymin + 1

	def x_of(x):
		return plot_left + (x - xmin) / (xmax - xmin) * plot_w

	def y_of(y):
		return plot_bottom - (y - ymin) / (ymax - ymin) * plot_h

	c.line(plot_left, plot_top, plot_left, plot_bottom)
	c.line(plot_left, plot_bottom, plot_right, plot_bottom)
	colors = {}
	for x, y, label, key in points:
		if key not in colors:
			colors[key] = PALETTE[len(colors) % len(PALETTE)]
		c.circle(x_of(x), y_of(y), 6, fill=colors[key])
		c.text(x_of(x) + 9, y_of(y) + 4, label, size=10)

	ly = plot_top - 15
	for key, color in colors.items():
		c.circle(plot_left + 10, ly - 4, 5, fill=color)
		c.text(plot_left + 22, ly, key, size=11)
		ly += 16

	c.text(plot_left + plot_w / 2, height - 45, x_label, size=12, anchor="middle")
	c.text(20, plot_top + plot_h / 2, y_label, size=12, anchor="middle", rotate=-90)
	if unit:
		c.text(plot_right, height - 8, unit, size=10, anchor="end", fill="#666")
	c.save(path)
