"""
ESP32-CAM (camra.ino) → laptop YOLO detection
=============================================
Reads MJPEG from http://<ESP32_IP>:81/stream and runs YOLOv8 locally.

Prerequisites:
  - Flash camra/camra.ino to ESP32-CAM, same WiFi as this PC
  - Note the IP from Serial Monitor after boot
  - pip install ultralytics opencv-python requests

Usage:
  python yolo_cam.py
  python yolo_cam.py --ip 192.168.1.42
"""
# python camra/yolo_cam.py --ip 172.20.95.80

import argparse
import os
import sys
import time

import cv2
import numpy as np
import requests
from ultralytics import YOLO

# Repo root has yolov8n.pt
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_CAM_IP = "192.168.1.100"  # change after camra.ino boots (Serial Monitor)
CONFIDENCE = 0.50
TARGET_CLASS = "person"

CLR_PERSON = (0, 0, 255)
CLR_OTHER = (0, 200, 100)
CLR_OVERLAY = (0, 229, 255)
FONT = cv2.FONT_HERSHEY_SIMPLEX


def stream_url(cam_ip: str) -> str:
    return f"http://{cam_ip}:81/stream"


def read_mjpeg_stream(url: str):
    """Yield decoded BGR frames from ESP32-CAM multipart MJPEG."""
    while True:
        try:
            resp = requests.get(url, stream=True, timeout=10)
            resp.raise_for_status()
            buf = b""
            for chunk in resp.iter_content(chunk_size=4096):
                buf += chunk
                start = buf.find(b"\xff\xd8")
                end = buf.find(b"\xff\xd9")
                if start != -1 and end != -1 and end > start:
                    jpg = buf[start : end + 2]
                    buf = buf[end + 2 :]
                    frame = cv2.imdecode(
                        np.frombuffer(jpg, dtype=np.uint8), cv2.IMREAD_COLOR
                    )
                    if frame is not None:
                        yield frame
        except Exception as e:
            print(f"[stream] {e} — retrying in 2s")
            time.sleep(2)


def draw_detections(frame, model, results):
    person_count = 0
    for result in results:
        for box in result.boxes:
            cls = int(box.cls[0])
            conf = float(box.conf[0])
            label = model.names[cls]
            x1, y1, x2, y2 = map(int, box.xyxy[0])
            is_target = label == TARGET_CLASS
            color = CLR_PERSON if is_target else CLR_OTHER
            thickness = 3 if is_target else 2
            cv2.rectangle(frame, (x1, y1), (x2, y2), color, thickness)
            tag = f"{label} {conf:.0%}"
            tw, th = cv2.getTextSize(tag, FONT, 0.6, 2)[0]
            cv2.rectangle(frame, (x1, y1 - th - 8), (x1 + tw + 4, y1), color, -1)
            cv2.putText(frame, tag, (x1 + 2, y1 - 4), FONT, 0.6, (255, 255, 255), 2)
            if is_target:
                person_count += 1
    return person_count


def overlay_hud(frame, fps_val, person_count, cam_ip):
    h, w = frame.shape[:2]
    cv2.rectangle(frame, (0, 0), (w, 36), (15, 20, 30), -1)
    cv2.putText(frame, f"FPS: {fps_val}", (8, 24), FONT, 0.65, CLR_OVERLAY, 2)
    cv2.putText(
        frame,
        f"{TARGET_CLASS}: {person_count}",
        (120, 24),
        FONT,
        0.65,
        CLR_PERSON if person_count else CLR_OVERLAY,
        2,
    )
    cv2.putText(frame, cam_ip, (w - 200, 24), FONT, 0.5, CLR_OVERLAY, 1)


def main():
    parser = argparse.ArgumentParser(description="ESP32-CAM stream + YOLO on laptop")
    parser.add_argument(
        "--ip",
        default=os.environ.get("ESP32_CAM_IP", DEFAULT_CAM_IP),
        help="ESP32-CAM IP (or set ESP32_CAM_IP env var)",
    )
    parser.add_argument("--conf", type=float, default=CONFIDENCE, help="YOLO confidence")
    args = parser.parse_args()

    url = stream_url(args.ip)
    model_path = os.path.join(ROOT, "yolov8n.pt")
    if not os.path.isfile(model_path):
        print(f"[error] Model not found: {model_path}")
        print("  Download runs on first YOLO() if ultralytics can fetch it.")
        model_path = "yolov8n.pt"

    print("=" * 56)
    print("  ESP32-CAM → Laptop YOLO")
    print("=" * 56)
    print(f"  Stream : {url}")
    print(f"  Target : {TARGET_CLASS} (conf >= {args.conf})")
    print("  Press Q in the video window to quit")
    print("=" * 56)

    print("[yolo] Loading model...")
    model = YOLO(model_path)
    print("[yolo] Ready — waiting for frames...")

    fps_time = time.time()
    fps_count = 0
    fps_val = 0

    try:
        for frame in read_mjpeg_stream(url):
            fps_count += 1
            if time.time() - fps_time >= 1.0:
                fps_val = fps_count
                fps_count = 0
                fps_time = time.time()

            results = model(frame, verbose=False, conf=args.conf)
            person_count = draw_detections(frame, model, results)
            overlay_hud(frame, fps_val, person_count, args.ip)

            cv2.imshow("ESP32-CAM — YOLO (Q to quit)", frame)
            if cv2.waitKey(1) & 0xFF == ord("q"):
                break
    except KeyboardInterrupt:
        pass

    cv2.destroyAllWindows()
    print("[exit] Stopped")


if __name__ == "__main__":
    main()
