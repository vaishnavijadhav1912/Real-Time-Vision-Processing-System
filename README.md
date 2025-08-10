Real-Time Vision System
Overview
A multi-threaded real-time computer-vision pipeline implemented in C on Linux. The system captures video frames from a camera, processes them in real-time using classic image-processing techniques, and stores/streams results while meeting strict timing constraints.

Features
Multi-threaded design using POSIX threads and semaphores.

SCHED_FIFO real-time scheduling with explicit CPU core affinity.

Video capture at 640×480 YUYV via V4L2.

Real-time image processing:

Grayscale conversion

3×3 box blur (noise reduction)

Sobel edge detection (feature extraction)

Ring-buffer frame management to decouple capture & processing rates.

Automated FTP upload of processed frames.

Syslog-based timing instrumentation for performance profiling.

System Architecture
The pipeline consists of:

Sequencer Thread – Triggers periodic tasks based on a fixed schedule.

Capture Thread – Acquires frames from the camera using V4L2.

Processing Thread – Converts frames to grayscale, applies blur & Sobel filter.

Write-back Thread – Saves processed images as .pgm.

FTP Thread – Uploads images to remote storage.

Requirements
Linux OS (tested on Ubuntu/Debian)

GCC compiler

V4L2-compatible camera

POSIX threads

FTP server for uploads
