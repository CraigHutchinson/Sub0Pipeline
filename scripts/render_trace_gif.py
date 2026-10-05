#!/usr/bin/env python3
"""Render a bounded Sub0Pipeline Chrome Trace capture as an animated DAG."""

from __future__ import annotations

import argparse
import json
from dataclasses import dataclass
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

WIDTH = 960
HEIGHT = 540
NODE_WIDTH = 132
NODE_HEIGHT = 72
STATUS_NAMES = {
    3: "DONE",
    4: "FAILED",
    5: "SKIPPED",
    6: "TIMED OUT",
    7: "CANCELLED",
}
COLORS = {
    "background": "#F5F8FC",
    "ink": "#172B4D",
    "muted": "#62748A",
    "line": "#C5D0DF",
    "edge": "#96A8BF",
    "active_edge": "#3982F6",
    "waiting_fill": "#FFFFFF",
    "waiting_border": "#C5D0DF",
    "running_fill": "#FFF3D6",
    "running_border": "#E9A23B",
    "done_fill": "#DDF6E8",
    "done_border": "#35A66F",
    "failed_fill": "#FFE1E1",
    "failed_border": "#D94F55",
    "skipped_fill": "#E9EDF3",
    "skipped_border": "#98A5B5",
    "panel": "#FFFFFF",
    "accent": "#1769D2",
    "track": "#E0E7F0",
}


@dataclass(frozen=True)
class JobTrace:
    job_id: int
    name: str
    started_at: float | None
    finished_at: float | None
    final_status: str


def _font(size: int, bold: bool = False) -> ImageFont.FreeTypeFont | ImageFont.ImageFont:
    suffix = "segoeuib.ttf" if bold else "segoeui.ttf"
    candidates = [
        Path("C:/Windows/Fonts") / suffix,
        Path("/usr/share/fonts/truetype/dejavu") /
        ("DejaVuSans-Bold.ttf" if bold else "DejaVuSans.ttf"),
        Path("/System/Library/Fonts/Supplemental") /
        ("Arial Bold.ttf" if bold else "Arial.ttf"),
    ]
    for candidate in candidates:
        if candidate.exists():
            return ImageFont.truetype(str(candidate), size)
    return ImageFont.load_default()


def _as_int(value: object, field: str) -> int:
    if not isinstance(value, int) or isinstance(value, bool):
        raise ValueError(f"trace field '{field}' must be an integer")
    return value


def _as_float(value: object, field: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ValueError(f"trace field '{field}' must be numeric")
    return float(value)


def _load_trace(path: Path, requested_run_id: int | None) -> tuple[
    dict[int, JobTrace], dict[tuple[int, int], float], int, float, float
]:
    document = json.loads(path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        raise ValueError("input must be a Chrome Trace JSON object")
    raw_events = document.get("traceEvents")
    if not isinstance(raw_events, list):
        raise ValueError("input must contain a Chrome Trace 'traceEvents' array")

    events: list[dict[str, object]] = [
        event for event in raw_events
        if isinstance(event, dict)
        and isinstance(event.get("args"), dict)
        and isinstance(event.get("ts"), (int, float))
    ]
    available_run_ids = sorted({
        _as_int(event["args"].get("run_id", 0), "run_id")
        for event in events
        if isinstance(event["args"], dict)
    })
    if not available_run_ids:
        raise ValueError("trace contains no events with run identities")
    run_id = requested_run_id if requested_run_id is not None else available_run_ids[0]
    if run_id not in available_run_ids:
        raise ValueError(f"run id {run_id} is not present in the trace")

    names: dict[int, str] = {}
    starts: dict[int, float] = {}
    finishes: dict[int, tuple[float, str]] = {}
    edges: dict[tuple[int, int], float] = {}
    selected_timestamps: list[float] = []

    for event in events:
        args = event["args"]
        if (not isinstance(args, dict)
                or _as_int(args.get("run_id", 0), "run_id") != run_id):
            continue
        timestamp = _as_float(event["ts"], "ts")
        selected_timestamps.append(timestamp)
        phase = event.get("ph")

        if "to_job_id" in args:
            edge = (
                _as_int(args["job_id"], "job_id"),
                _as_int(args["to_job_id"], "to_job_id"),
            )
            edges[edge] = min(timestamp, edges.get(edge, timestamp))
            names.setdefault(edge[0], str(args.get("from", f"job {edge[0]}")))
            names.setdefault(edge[1], str(args.get("to", f"job {edge[1]}")))
            continue

        job_id = _as_int(args["job_id"], "job_id")
        names[job_id] = str(event.get("name", f"job {job_id}"))
        if phase == "B":
            starts[job_id] = timestamp
        elif phase in {"E", "i"} and "status" in args:
            status_code = _as_int(args["status"], "status")
            finishes[job_id] = (
                timestamp,
                STATUS_NAMES.get(status_code, "FINISHED"),
            )

    if not names or not selected_timestamps:
        raise ValueError(f"run id {run_id} has no job events")

    jobs = {
        job_id: JobTrace(
            job_id=job_id,
            name=name,
            started_at=starts.get(job_id),
            finished_at=finishes.get(job_id, (None, "WAITING"))[0],
            final_status=finishes.get(job_id, (None, "WAITING"))[1],
        )
        for job_id, name in names.items()
    }
    ordered_edges = {
        edge: timestamp
        for edge, timestamp in edges.items()
        if edge[0] in jobs and edge[1] in jobs
    }
    return jobs, ordered_edges, run_id, min(selected_timestamps), max(selected_timestamps)


def _layout(
    jobs: dict[int, JobTrace], edges: dict[tuple[int, int], float]
) -> dict[int, tuple[int, int]]:
    successors = {job_id: set() for job_id in jobs}
    indegree = {job_id: 0 for job_id in jobs}
    for source, target in edges:
        if target not in successors[source]:
            successors[source].add(target)
            indegree[target] += 1

    ready = sorted(job_id for job_id, count in indegree.items() if count == 0)
    levels = {job_id: 0 for job_id in ready}
    order: list[int] = []
    while ready:
        job_id = ready.pop(0)
        order.append(job_id)
        for target in sorted(successors[job_id]):
            levels[target] = max(levels.get(target, 0), levels[job_id] + 1)
            indegree[target] -= 1
            if indegree[target] == 0:
                ready.append(target)
                ready.sort()
    if len(order) != len(jobs):
        raise ValueError("trace dependencies contain a cycle")

    by_level: dict[int, list[int]] = {}
    for job_id in order:
        by_level.setdefault(levels[job_id], []).append(job_id)

    max_level = max(levels.values(), default=0)
    left = 52
    right = 690
    top = 160
    bottom = 390
    step_x = (right - left - NODE_WIDTH) / max(1, max_level)
    positions: dict[int, tuple[int, int]] = {}
    for level, job_ids in by_level.items():
        spacing = (bottom - top) / (len(job_ids) + 1)
        for index, job_id in enumerate(job_ids, start=1):
            positions[job_id] = (
                round(left + level * step_x),
                round(top + index * spacing - NODE_HEIGHT / 2),
            )
    return positions


def _shorten(draw: ImageDraw.ImageDraw, text: str, font: ImageFont.ImageFont,
             max_width: int) -> str:
    if draw.textbbox((0, 0), text, font=font)[2] <= max_width:
        return text
    while text and draw.textbbox((0, 0), text + "...", font=font)[2] > max_width:
        text = text[:-1]
    return text + "..."


def _caption(completed: int, active: int, job_count: int) -> str:
    if completed == job_count:
        return "Every job reached a terminal state; statuses remain visible."
    if active > 1:
        return "Independent branches are executing in parallel."
    if completed:
        return "Resolved dependencies release the next job."
    if active:
        return "A job starts only after its prerequisites are met."
    return "An observer turns runtime events into a bounded trace."


def _frame(
    jobs: dict[int, JobTrace],
    edges: dict[tuple[int, int], float],
    positions: dict[int, tuple[int, int]],
    run_id: int,
    now: float,
    first_timestamp: float,
    last_timestamp: float,
) -> Image.Image:
    image = Image.new("RGB", (WIDTH, HEIGHT), COLORS["background"])
    draw = ImageDraw.Draw(image)
    title_font = _font(25, bold=True)
    body_font = _font(13)
    label_font = _font(11, bold=True)
    node_font = _font(14, bold=True)
    small_font = _font(10)

    draw.text((48, 38), "Sub0Pipeline", font=label_font, fill=COLORS["accent"])
    draw.text((48, 66), "Define once. Execute by dependency.", font=title_font,
              fill=COLORS["ink"])
    draw.text((48, 100),
              "Independent jobs overlap; successors wait for their prerequisites.",
              font=body_font, fill=COLORS["muted"])

    for (source, target), resolved_at in edges.items():
        source_x, source_y = positions[source]
        target_x, target_y = positions[target]
        x1 = source_x + NODE_WIDTH
        y1 = source_y + NODE_HEIGHT // 2
        x2 = target_x
        y2 = target_y + NODE_HEIGHT // 2
        mid_x = (x1 + x2) // 2
        color = COLORS["active_edge"] if now >= resolved_at else COLORS["edge"]
        draw.line((x1, y1, mid_x, y1, mid_x, y2, x2 - 7, y2),
                  fill=color, width=3 if now >= resolved_at else 2,
                  joint="curve")
        draw.polygon(
            ((x2, y2), (x2 - 9, y2 - 5), (x2 - 9, y2 + 5)),
            fill=color,
        )

    active_count = 0
    completed_count = 0
    for job_id, job in jobs.items():
        started = job.started_at is not None and now >= job.started_at
        finished = job.finished_at is not None and now >= job.finished_at
        if finished:
            state = job.final_status.lower()
            completed_count += 1
        elif started:
            state = "running"
            active_count += 1
        else:
            state = "waiting"

        style_state = "failed" if state in {"timed out", "cancelled"} else state
        fill = COLORS.get(f"{style_state}_fill", COLORS["waiting_fill"])
        border = COLORS.get(f"{style_state}_border", COLORS["waiting_border"])
        x, y = positions[job_id]
        draw.rounded_rectangle(
            (x, y, x + NODE_WIDTH, y + NODE_HEIGHT),
            radius=13,
            fill=fill,
            outline=border,
            width=3 if state == "running" else 2,
        )
        name = _shorten(draw, job.name, node_font, NODE_WIDTH - 18)
        draw.text((x + NODE_WIDTH // 2, y + 25), name, font=node_font,
                  fill=COLORS["ink"], anchor="mm")
        draw.text((x + NODE_WIDTH // 2, y + 51), state.upper(),
                  font=label_font, fill=border, anchor="mm")

    panel = (735, 132, 916, 410)
    draw.rounded_rectangle(panel, radius=16, fill=COLORS["panel"],
                           outline=COLORS["track"], width=1)
    draw.text((753, 158), "TRACE PLAYBACK", font=label_font,
              fill=COLORS["accent"])
    draw.text((753, 188), f"Run {run_id}", font=body_font, fill=COLORS["ink"])
    draw.line((753, 208, 898, 208), fill=COLORS["track"], width=1)
    draw.text((753, 232), "JOBS COMPLETE", font=small_font, fill=COLORS["muted"])
    draw.text((753, 253), f"{completed_count} / {len(jobs)}",
              font=_font(20, bold=True), fill=COLORS["ink"])
    draw.text((753, 294), "RUNNING NOW", font=small_font, fill=COLORS["muted"])
    draw.text((753, 315), str(active_count), font=_font(20, bold=True),
              fill=COLORS["ink"])
    resolved = sum(now >= timestamp for timestamp in edges.values())
    draw.text((753, 356), "EDGES RESOLVED", font=small_font, fill=COLORS["muted"])
    draw.text((753, 377), f"{resolved} / {len(edges)}",
              font=_font(16, bold=True), fill=COLORS["ink"])

    draw.text((48, 438), _caption(completed_count, active_count, len(jobs)),
              font=body_font, fill=COLORS["ink"])
    draw.rounded_rectangle((48, 475, 912, 485), radius=5, fill=COLORS["track"])
    span = last_timestamp - first_timestamp
    progress = (
        float(now >= last_timestamp)
        if span <= 0
        else min(1.0, max(0.0, (now - first_timestamp) / span))
    )
    draw.rounded_rectangle(
        (48, 475, 48 + round(864 * progress), 485),
        radius=5,
        fill=COLORS["active_edge"],
    )
    draw.text((48, 510),
              f"OPT-IN TRACE   |   {len(jobs)} JOBS   |   {len(edges)} DEPENDENCIES",
              font=label_font, fill=COLORS["muted"])
    draw.text((912, 510), f"{round(progress * 100):3d}%",
              font=label_font, fill=COLORS["accent"], anchor="ra")
    return image


def render(trace_path: Path, output_path: Path, run_id: int | None) -> int:
    jobs, edges, selected_run, first_timestamp, last_timestamp = _load_trace(
        trace_path, run_id
    )
    positions = _layout(jobs, edges)
    span = max(1.0, last_timestamp - first_timestamp)
    intro_time = first_timestamp - span * 0.12
    frame_count = 30
    timestamps = [
        intro_time + (last_timestamp - intro_time) * index / (frame_count - 1)
        for index in range(frame_count)
    ]
    frames = [
        _frame(
            jobs, edges, positions, selected_run, timestamp,
            first_timestamp, last_timestamp,
        )
        for timestamp in timestamps
    ]
    frames = [frames[0]] * 5 + frames + [frames[-1]] * 8

    palette = frames[len(frames) // 2].quantize(
        colors=256, method=Image.Quantize.MEDIANCUT
    )
    indexed_frames = [
        frame.quantize(palette=palette, dither=Image.Dither.NONE)
        for frame in frames
    ]
    output_path.parent.mkdir(parents=True, exist_ok=True)
    indexed_frames[0].save(
        output_path,
        format="GIF",
        save_all=True,
        append_images=indexed_frames[1:],
        duration=100,
        loop=0,
        disposal=2,
        optimize=False,
    )
    with Image.open(output_path) as saved:
        return saved.n_frames


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("trace", type=Path, help="Chrome Trace JSON from trace_capture")
    parser.add_argument("output", type=Path, help="destination animated GIF")
    parser.add_argument("--run-id", type=int, help="select a run when the trace has several")
    args = parser.parse_args()
    try:
        frame_count = render(args.trace, args.output, args.run_id)
    except (OSError, ValueError, KeyError, TypeError, json.JSONDecodeError) as error:
        parser.error(str(error))
    print(f"Wrote {args.output} ({frame_count} frames)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
