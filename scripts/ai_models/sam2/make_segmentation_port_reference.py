#!/usr/bin/env python3
"""Reference masks for the port check of the Alcedo Studio segmentation service (SM3.A.2).

The Rust test `rust/puerh_mind/tests/segmentation_port_check.rs` runs the sidecar segmentation
engine on the same pixels and prompts and compares its masks with the masks of this script.

For each photo of --photos (8-bit RGB, long edge at most 4096 px; the lab input set
`build/tmp/explore/e4/input_4096/photos`), the script:

1. writes the exact RGB8 pixels that it uses (`<photo>.rgb`, row-major), so that both sides read
   the same values and no JPEG decoder difference enters the comparison;
2. preprocesses them as the lab MVP does (torchvision `ToTensor`, `Resize((1024, 1024),
   antialias=True)`, ImageNet `Normalize`) and runs `point_encoder.onnx` once on ONNX Runtime CPU;
3. runs `point_decoder.onnx` for each prompt and applies the MVP selection rules (master plan
   section 8.17.3): a click returns tokens 1 to 3 sorted by area with the best-scored token
   selected; another prompt returns token 0 when its stability is at least 0.98, else the
   best-scored token 1 to 3; logits clipped to +-32.

Prompts per photo (normalized coordinates of the lab quality prompts,
`build/tmp/sm1/quality/prompts.json`): every point set (one point is a click), every box, a
4-point stroke across the center of the first box, and a subtract click inside the first box with
the selected box logits as `mask_input`.

Output (--out): `manifest.json` and little-endian float32 files (`*.f32`) of the mask input and
the logits of every returned mask.

Task masks (SM3.C and SM3.D), when --lab-dir, --head-onnx, --head-json, and --subject-onnx are
given: the script runs the people and sky head on the encoder output and S3OD on the photo and on
each person crop with the functions of the lab repository (`alcedo-ai-mask-lab`, the code of the
MVP `mask_pipeline.py`): `evaluate_quality_photos.sky_regions` and `region_points` (Sky),
`head_model.decode_people` with an overlap limit of 0.5 (People list), `s3od_graphs.letterbox`,
`select_mask`, and `unpad` (Subject and each person crop), and `mask_pipeline.person_crop_rect`,
`person_crop_image`, `subject_fraction`, and `PERSON_IN_SUBJECT_FRACTION` (SM3.D). It writes the
logits of each Sky region, the Subject alpha, and for each listed person the crop rectangle, the
crop alpha, `subject_fraction`, and `in_subject`. The People list follows the SM3.C plan: the
boxes with a score of at least the list threshold, else the best box when the person presence
passed (decision C2), sorted from left to right by the box center (C3). The Rust test makes the
Sky union itself.

Sky head (SM3.E), when --sky-head-onnx is also given: the script runs the sky head on ONNX Runtime
CPU with the encoder outputs and the encoder input of the photo, and writes its 1024 x 1024 sky
alpha (`<photo>_sky_alpha.f32`), its sky share (the share of the values >= 0.5), and whether the
share reaches SKY_FOUND_SHARE (`tasks.sky_head`). The MVP Sky (`tasks.sky`) stays for the backends
without a sky head (Core ML until AlcedoStudio issue #538).
"""

from __future__ import annotations

import argparse
import hashlib
import importlib.util
import json
import sys
from pathlib import Path

import numpy as np
import onnxruntime as ort
import torch
from PIL import Image, ImageOps
from torchvision.transforms import Normalize, Resize, ToTensor

IMAGE_SIZE = 1024
LOGIT_SIZE = 256
LOGIT_LIMIT = 32.0
STABILITY_DELTA = 0.05
STABILITY_THRESHOLD = 0.98
MEAN = (0.485, 0.456, 0.406)
STD = (0.229, 0.224, 0.225)
# The "sky found" share of the sky head (rust/puerh_mind task_masks::SKY_FOUND_SHARE).
SKY_FOUND_SHARE = 5e-4
SKY_HEAD_INPUTS = ["image_embed", "high_res_feats_0", "high_res_feats_1", "pixel_values"]


def file_sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as stream:
        for block in iter(lambda: stream.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def read_rgb8(path: Path) -> np.ndarray:
    with Image.open(path) as image:
        rgb = ImageOps.exif_transpose(image).convert("RGB")
        return np.asarray(rgb, dtype=np.uint8).copy()


def preprocess(rgb8: np.ndarray) -> np.ndarray:
    tensor = ToTensor()(rgb8)
    tensor = Resize((IMAGE_SIZE, IMAGE_SIZE), antialias=True)(tensor)
    tensor = Normalize(list(MEAN), list(STD))(tensor)
    return tensor.unsqueeze(0).numpy().astype(np.float32)


def model_prompt(points: list[list[float]], box: list[float] | None):
    coords, labels = [], []
    if box is not None:
        x0, y0, x1, y1 = box
        coords += [[min(x0, x1) * IMAGE_SIZE, min(y0, y1) * IMAGE_SIZE],
                   [max(x0, x1) * IMAGE_SIZE, max(y0, y1) * IMAGE_SIZE]]
        labels += [2.0, 3.0]
    coords += [[x * IMAGE_SIZE, y * IMAGE_SIZE] for x, y, _ in points]
    labels += [float(label) for _, _, label in points]
    return np.array([coords], np.float32), np.array([labels], np.float32)


def level(logits: np.ndarray, scores: np.ndarray, token: int) -> dict:
    clipped = np.clip(logits[token], -LOGIT_LIMIT, LOGIT_LIMIT).astype(np.float32)
    return {"token": token, "score": float(scores[token]), "area": int((clipped > 0).sum()),
            "logits": clipped}


def select_masks(logits: np.ndarray, scores: np.ndarray, is_click: bool):
    if is_click:
        best = 1 + int(np.argmax(scores[1:]))
        levels = sorted((level(logits, scores, token) for token in (1, 2, 3)),
                        key=lambda item: (item["area"], item["token"]))
        return levels, next(i for i, item in enumerate(levels) if item["token"] == best)
    first = logits[0]
    inner = int((first > STABILITY_DELTA).sum())
    outer = int((first > -STABILITY_DELTA).sum())
    stability = inner / outer if outer > 0 else 1.0
    token = 0 if stability >= STABILITY_THRESHOLD else 1 + int(np.argmax(scores[1:]))
    return [level(logits, scores, token)], 0


def prompts_for(entry: dict) -> list[dict]:
    prompts = [{"name": f"points_{item['name']}", "points": item["points"], "box": None}
               for item in entry.get("points", [])]
    prompts += [{"name": f"box_{item['name']}", "points": [], "box": item["box"]}
                for item in entry.get("boxes", [])]
    if entry.get("boxes"):
        x0, y0, x1, y1 = entry["boxes"][0]["box"]
        cy = (y0 + y1) / 2
        stroke = [[x0 + (x1 - x0) * (0.4 + 0.2 * k / 3), cy, 1] for k in range(4)]
        prompts.append({"name": "stroke_box_center", "points": stroke, "box": None})
        prompts.append({"name": "box_then_subtract", "points": [[(x0 + x1) / 2, y0 + 0.1 * (y1 - y0), 0]],
                        "box": entry["boxes"][0]["box"], "mask_input_from": f"box_{entry['boxes'][0]['name']}"})
    return prompts


class TaskReference:
    """The task masks of the SM3.C plan with the functions of the lab repository."""

    def __init__(self, lab_dir: Path, head_onnx: Path, head_json: Path, subject_onnx: Path,
                 decode) -> None:
        scripts = lab_dir / "scripts"
        for directory in (scripts / "poc" / "people_sky_head",
                          scripts / "poc" / "sam_feature_localization",
                          scripts / "poc" / "ai_mask_mvp"):
            sys.path.insert(0, str(directory))
        import evaluate_quality_photos as quality
        import head_model
        import mask_pipeline

        # s3od_graphs by its file path: its directory has another `compare_onnx_outputs.py`.
        spec = importlib.util.spec_from_file_location(
            "s3od_graphs", scripts / "ai_models" / "s3od" / "s3od_graphs.py")
        self.s3od = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(self.s3od)
        self.quality = quality
        self.head_model = head_model
        self.mask_pipeline = mask_pipeline
        self.decode = decode
        validation = json.loads(head_json.read_text())["validation"]
        self.thresholds = {
            "sky_presence": float(validation["presence_best_f1_sky"]["threshold"]),
            "person_presence": float(validation["presence_best_f1_person"]["threshold"]),
            "people_list": float(validation["person_list_best_f1_nms"]["threshold"]),
        }
        self.head = ort.InferenceSession(str(head_onnx), providers=["CPUExecutionProvider"])
        self.subject = ort.InferenceSession(str(subject_onnx),
                                            providers=["CPUExecutionProvider"])
        self.sha256 = {"head": file_sha256(head_onnx), "head_json": file_sha256(head_json),
                       "subject": file_sha256(subject_onnx)}

    def subject_alpha(self, rgb8: np.ndarray) -> tuple[np.ndarray, float]:
        """S3OD on an RGB8 image (MVP select_subject): the alpha without padding and its score."""
        pixels, info = self.s3od.letterbox(np.ascontiguousarray(rgb8), self.s3od.IMAGE_SIZE)
        masks, iou_scores = self.subject.run(None, {"pixel_values": pixels})
        alpha, index = self.s3od.select_mask(masks, iou_scores)
        alpha = np.ascontiguousarray(self.s3od.unpad(alpha[None], info)[0], np.float32)
        return alpha, float(iou_scores[0, index])

    def run(self, rgb8: np.ndarray, image_embed: np.ndarray, out: Path, stem: str) -> dict:
        sigmoid = self.quality.sigmoid
        outputs = dict(zip(self.head_model.OUTPUT_NAMES,
                           self.head.run(None, {"image_embed": image_embed})))
        presence = sigmoid(outputs["presence_logits"][0])
        record = {"sky_presence": float(presence[0]), "person_presence": float(presence[1])}

        # Sky (MVP select_sky).
        sky = {"found": False, "regions": []}
        if presence[0] >= self.thresholds["sky_presence"]:
            probability = sigmoid(outputs["sky_logits"][0, 0])
            regions = self.quality.sky_regions(probability, self.quality.SKY_POINTS_PER_REGION)
            for index, region in enumerate(regions):
                points = self.quality.region_points(probability, region)
                logits = self.decode(np.array([points], np.float32),
                                     np.array([[1.0] * len(points)], np.float32))
                logits_file = f"{stem}_sky{index}.f32"
                (out / logits_file).write_bytes(logits.astype("<f4").tobytes())
                sky["regions"].append({"points": points, "logits": logits_file})
            sky["found"] = bool(regions)
        record["sky"] = sky

        # Subject (MVP select_subject with S3OD).
        subject_alpha, subject_score = self.subject_alpha(rgb8)
        alpha_file = f"{stem}_subject_alpha.f32"
        (out / alpha_file).write_bytes(subject_alpha.astype("<f4").tobytes())
        record["subject"] = {"width": int(subject_alpha.shape[1]),
                             "height": int(subject_alpha.shape[0]), "score": subject_score,
                             "alpha": alpha_file}

        # People (SM3.D: the SM3.C list rules C2 and C3, S3OD on each person crop).
        lab = self.mask_pipeline
        height, width = rgb8.shape[:2]
        torch_outputs = {key: torch.from_numpy(value) for key, value in outputs.items()}
        decoded = self.head_model.decode_people(torch_outputs, nms_iou=0.5)[0]
        boxes = [{"box_cells": [float(v) for v in box], "score": float(score)}
                 for box, score in zip(decoded["boxes"].tolist(), decoded["scores"].tolist())]
        people = {"found": False, "all_boxes": boxes, "people": []}
        if presence[1] >= self.thresholds["person_presence"]:
            listed = [box for box in boxes if box["score"] >= self.thresholds["people_list"]]
            if not listed and boxes:
                listed = [boxes[0]]
            listed.sort(key=lambda box: ((box["box_cells"][0] + box["box_cells"][2]) / 2,
                                         -box["score"]))
            for index, box in enumerate(listed):
                rect = lab.person_crop_rect(box["box_cells"], width, height)
                crop = lab.person_crop_image(rgb8, rect, self.s3od)
                alpha, score = self.subject_alpha(crop)
                fraction = lab.subject_fraction(subject_alpha, box["box_cells"])
                person_file = f"{stem}_person{index}_alpha.f32"
                (out / person_file).write_bytes(alpha.astype("<f4").tobytes())
                people["people"].append({
                    **box, "rect": [int(v) for v in rect],
                    "crop_width": int(crop.shape[1]), "crop_height": int(crop.shape[0]),
                    "alpha": person_file, "alpha_width": int(alpha.shape[1]),
                    "alpha_height": int(alpha.shape[0]), "alpha_score": score,
                    "subject_fraction": fraction,
                    "in_subject": fraction >= lab.PERSON_IN_SUBJECT_FRACTION})
            people["found"] = bool(listed)
        record["people"] = people
        return record


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--onnx-dir", type=Path, required=True,
                        help="directory with point_encoder.onnx and point_decoder.onnx")
    parser.add_argument("--photos", type=Path, required=True)
    parser.add_argument("--prompts", type=Path, required=True)
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--lab-dir", type=Path, help="alcedo-ai-mask-lab checkout (task masks)")
    parser.add_argument("--head-onnx", type=Path)
    parser.add_argument("--head-json", type=Path)
    parser.add_argument("--subject-onnx", type=Path)
    parser.add_argument("--sky-head-onnx", type=Path, help="sky_head.onnx (SM3.E)")
    args = parser.parse_args()
    task_arguments = (args.lab_dir, args.head_onnx, args.head_json, args.subject_onnx)
    if any(task_arguments) and not all(task_arguments):
        parser.error("the task masks need --lab-dir, --head-onnx, --head-json, and --subject-onnx")
    if args.sky_head_onnx is not None and not all(task_arguments):
        parser.error("--sky-head-onnx needs the task mask arguments")

    encoder_path = args.onnx_dir / "point_encoder.onnx"
    decoder_path = args.onnx_dir / "point_decoder.onnx"
    encoder = ort.InferenceSession(str(encoder_path), providers=["CPUExecutionProvider"])
    decoder = ort.InferenceSession(str(decoder_path), providers=["CPUExecutionProvider"])
    entries = {entry["file"]: entry for entry in json.loads(args.prompts.read_text())["images"]}
    args.out.mkdir(parents=True, exist_ok=True)

    manifest = {"onnxruntime": ort.__version__, "torch": torch.__version__,
                "encoder_sha256": file_sha256(encoder_path),
                "decoder_sha256": file_sha256(decoder_path), "photos": []}
    features: list = []

    def decode_single(coords: np.ndarray, labels: np.ndarray) -> np.ndarray:
        """One decoder run without a mask input on the current photo; the selected single mask."""
        image_embed, feats_0, feats_1 = features
        logits, scores, _ = decoder.run(None, {
            "image_embed": image_embed, "high_res_feats_0": feats_0, "high_res_feats_1": feats_1,
            "point_coords": coords, "point_labels": labels,
            "mask_input": np.zeros((1, 1, LOGIT_SIZE, LOGIT_SIZE), np.float32),
            "has_mask_input": np.array([0.0], np.float32)})
        levels, selected = select_masks(logits[0], scores[0], False)
        return levels[selected]["logits"]

    tasks = None
    if all(task_arguments):
        tasks = TaskReference(args.lab_dir, args.head_onnx, args.head_json, args.subject_onnx,
                              decode_single)
        manifest["task_sha256"] = tasks.sha256
        manifest["task_thresholds"] = tasks.thresholds
        manifest["person_crop_margin"] = tasks.mask_pipeline.PERSON_CROP_MARGIN
        manifest["person_in_subject_fraction"] = tasks.mask_pipeline.PERSON_IN_SUBJECT_FRACTION
    sky_head = None
    if args.sky_head_onnx is not None:
        sky_head = ort.InferenceSession(str(args.sky_head_onnx),
                                        providers=["CPUExecutionProvider"])
        manifest["sky_head_sha256"] = file_sha256(args.sky_head_onnx)
        manifest["sky_found_share"] = SKY_FOUND_SHARE
    for photo_path in sorted(args.photos.iterdir()):
        entry = entries.get(photo_path.name)
        if entry is None:
            continue
        stem = photo_path.stem
        rgb8 = read_rgb8(photo_path)
        height, width = rgb8.shape[:2]
        (args.out / f"{stem}.rgb").write_bytes(rgb8.tobytes())
        pixel_values = preprocess(rgb8)
        image_embed, feats_0, feats_1 = encoder.run(None, {"pixel_values": pixel_values})
        features[:] = [image_embed, feats_0, feats_1]
        selected_by_name: dict[str, np.ndarray] = {}
        photo = {"name": stem, "width": int(width), "height": int(height),
                 "pixels": f"{stem}.rgb", "prompts": []}
        for index, prompt in enumerate(prompts_for(entry)):
            coords, labels = model_prompt(prompt["points"], prompt["box"])
            mask_input = selected_by_name.get(prompt.get("mask_input_from", ""))
            outputs = decoder.run(None, {
                "image_embed": image_embed, "high_res_feats_0": feats_0, "high_res_feats_1": feats_1,
                "point_coords": coords, "point_labels": labels,
                "mask_input": (mask_input[None, None] if mask_input is not None
                               else np.zeros((1, 1, LOGIT_SIZE, LOGIT_SIZE), np.float32)),
                "has_mask_input": np.array([1.0 if mask_input is not None else 0.0], np.float32)})
            logits, scores = outputs[0][0], outputs[1][0]
            is_click = prompt["box"] is None and len(prompt["points"]) == 1
            levels, selected = select_masks(logits, scores, is_click)
            selected_by_name[prompt["name"]] = levels[selected]["logits"]
            record = {"name": prompt["name"], "points": prompt["points"], "box": prompt["box"],
                      "mask_input": None, "selected_index": selected, "masks": []}
            if mask_input is not None:
                record["mask_input"] = f"{stem}_{index}_mask_input.f32"
                (args.out / record["mask_input"]).write_bytes(mask_input.astype("<f4").tobytes())
            for level_index, item in enumerate(levels):
                logits_file = f"{stem}_{index}_level{level_index}.f32"
                (args.out / logits_file).write_bytes(item["logits"].astype("<f4").tobytes())
                record["masks"].append({"token": item["token"], "score": item["score"],
                                        "area": item["area"], "logits": logits_file})
            photo["prompts"].append(record)
        if tasks is not None:
            photo["tasks"] = tasks.run(rgb8, image_embed, args.out, stem)
        if sky_head is not None:
            feeds = dict(zip(SKY_HEAD_INPUTS, (image_embed, feats_0, feats_1, pixel_values)))
            sky_alpha = np.ascontiguousarray(sky_head.run(None, feeds)[0][0, 0], np.float32)
            share = float(np.count_nonzero(sky_alpha >= 0.5)) / sky_alpha.size
            alpha_file = f"{stem}_sky_alpha.f32"
            (args.out / alpha_file).write_bytes(sky_alpha.astype("<f4").tobytes())
            photo["tasks"]["sky_head"] = {"alpha": alpha_file, "size": int(sky_alpha.shape[0]),
                                          "share": share, "found": share >= SKY_FOUND_SHARE}
        manifest["photos"].append(photo)
        print(f"{stem}: {width}x{height}, {len(photo['prompts'])} prompts")
    (args.out / "manifest.json").write_text(json.dumps(manifest, indent=1))


if __name__ == "__main__":
    main()
