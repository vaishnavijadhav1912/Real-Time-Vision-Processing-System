
 /*
      Author Names: Vaishnavi Jadhav, Anuja Joshi

      RTES PROJECT 
 
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <syslog.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/utsname.h>
#include <linux/videodev2.h>
#include <math.h>    



#define CAM_DEV      "/dev/video0"
#define W            640
#define H            480
#define PIX          V4L2_PIX_FMT_YUYV           
#define DRV_BUFS     4                           
#define RING_SIZE    4                           
#define MAX_FRAMES   1801                     



#define FTP_HOST  "10.0.0.224"
#define FTP_PATH  "/"
#define FTP_USER  "camera"
#define FTP_PASS  "12345"


#define SEQ_PRIO 99
#define CAP_PRIO 97
#define PROC_PRIO 94
#define WR_PRIO  90
#define SEQ_CORE 1
#define WRK_CORE 0
#define FTP_CORE 2

//#define PERIOD_NS 1000000000L   /* 1 Hz */
//#define NSECS 1000000000L

#define PERIOD_NS 100000000L    /* 10 Hz  (0.1 s) */

#define NSECS 1000000000L


#define FTP_STRIDE   ((PERIOD_NS == 100000000L) ? 10 : 1)

static char uname_buf[256] = "unknown";


#define FTP_QUEUE_SIZE 16

static char *ftp_queue[FTP_QUEUE_SIZE];

static int ftp_q_head = 0;
static int ftp_q_tail = 0;

static pthread_mutex_t ftp_q_lock = PTHREAD_MUTEX_INITIALIZER;

static sem_t sem_ftp;



typedef struct { void *start; size_t len; } vbuf_t;


static int cam_fd = -1;

static vbuf_t drv[DRV_BUFS];

/*
 * fslot_t: Structure representing a single slot in the ring buffer.
 *
 * - gray: Pointer to the grayscale image data (W × H bytes).
 * - ts: Timestamp of when the frame was captured (CLOCK_MONOTONIC).
 * - full: Indicates whether the slot contains a valid, processed frame.
 *
 * This structure is used for inter-thread communication between
 * capture, processing, and write threads in the real-time vision pipeline.
 */

typedef struct {
    uint8_t *gray;          
    struct timespec ts;    
    bool full;
} fslot_t;


/*
 * ring: Array of frame slots acting as a circular buffer for image data.
 * head: Index where the next captured frame will be written.
 * tail: Index of the next frame to be read/processed.
 *
 * The capture, processing, and write threads use this ring buffer
 * to pass grayscale image frames and their metadata in a synchronized manner.
 * 'head' and 'tail' are marked volatile to ensure proper memory visibility across threads.
 */
static fslot_t ring[RING_SIZE];
static volatile size_t head = 0;   
static volatile size_t tail = 0;   



/*
 * sem_cap: Signals the capture thread to grab a new frame.

 * sem_proc: Signals the processing thread that a new frame is ready to process.

 * sem_wr: Signals the write thread that a processed frame is ready to be saved.
 *
 * These semaphores coordinate execution between the sequencer, capture, processing,
 * and write threads to maintain a synchronized real-time image processing pipeline.
 */
static sem_t sem_cap;
static sem_t sem_proc;
static sem_t sem_wr;


/*
 * This function prints an error message and stops the program.
 *
 * It takes a formatted string (like printf) and uses it to create an error message.
 * 
 * It logs the error to syslog and also prints it to the terminal using perror.
 * 
 * Finally, it exits the program with a failure status.
 */
static void die(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char msg[256]; vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    syslog(LOG_ERR, "FATAL: %s (%d:%s)", msg, errno, strerror(errno));
    perror("fatal"); exit(EXIT_FAILURE);
}


/*
 * This function logs a warning message without stopping the program.
 *
 * It takes a formatted string (like printf), creates a message, 
 * 
 * and sends it to the system log with a warning level.
 */
static void warn(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    char msg[256]; vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    syslog(LOG_WARNING, "%s", msg);
}



/*
 * This function sets the calling thread to real-time FIFO scheduling with a given priority.
 *
 * If setting the SCHED_FIFO policy fails, it logs a warning with the requested priority value.
 */
static void set_rt(int prio)
{
    struct sched_param sp = { .sched_priority = prio };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp))
        warn("SCHED_FIFO prio %d failed", prio);
}




/*
 * This function pins the current thread to a specific CPU core.
 *
 * It sets the thread's CPU affinity so it always runs on the given core.
 * 
 * If setting the affinity fails, it logs a warning with the core number.
 */
static void pin(int core)
{
    cpu_set_t s; CPU_ZERO(&s); 
    CPU_SET(core, &s);
    if (pthread_setaffinity_np(pthread_self(), sizeof s, &s))
        warn("affinity core %d failed", core);
}


/*
 * This function adds a given number of nanoseconds to a timespec structure.
 *
 * If the nanoseconds exceed one second (NSECS), it rolls over the extra 
 * 
 * time into the seconds field to keep the timespec valid.
 */
static void add_ns(struct timespec *t, long ns)
{
    t->tv_nsec += ns;
    while (t->tv_nsec >= NSECS) { t->tv_sec++; t->tv_nsec -= NSECS; }
}



/*
 * This function writes the entire buffer to a file descriptor, handling interruptions.
 *
 * It keeps trying until all bytes are written or an error (other than EINTR) occurs.
 * 
 * Returns the total number of bytes written, or -1 on failure.
 */
static long w_all(int fd, const void *buf, long len)
{
    const uint8_t *p = buf; long left = len;
    while (left) {
        long w = write(fd, p, left);
        if (w < 0) { if (errno == EINTR) continue; return -1; }
        left -= w; p += w;
    }
    return len;
}


/*
 * This function calculates the time difference between two timestamps in nanoseconds.
 *
 * It takes two timespec structures and returns the difference a - b as a long integer.
 */
static inline long ns_diff(const struct timespec *a, const struct timespec *b)
{
    return (a->tv_sec - b->tv_sec) * NSECS + (a->tv_nsec - b->tv_nsec);
}



/*
 * This function converts time from nanoseconds to milliseconds.
 *
 * It takes a value in nanoseconds and returns the equivalent time in milliseconds as a double.
 */
static inline double ns_to_ms(long ns)
{
    return ns / 1000000.0;
}


static void ftp_upload(const char *path)
{
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);  

    char cmd[256];
    snprintf(cmd, sizeof cmd,
             "curl --silent --ftp-pasv -T '%s' "
             "ftp://%s:%s@%s%s",
             path, FTP_USER, FTP_PASS, FTP_HOST, FTP_PATH);

    syslog(LOG_INFO, "FTP uploading %s...", path);

    int rc = system(cmd);

    clock_gettime(CLOCK_MONOTONIC, &t1);  

    double ms = ns_to_ms(ns_diff(&t1, &t0));  

    if (rc)
        syslog(LOG_WARNING, "FTP upload failed for %s (rc=%d, time=%.2f ms)", path, rc, ms);
    else
        syslog(LOG_INFO,    "FTP uploaded %.2f ms", ms);
        syslog(LOG_INFO,    "FTP uploaded path %s ", path);
}


/*
 * This function uploads a file to a remote FTP server using the curl command.
 *
 * It records the start and end time of the upload using CLOCK_MONOTONIC to calculate how long the upload took.
 * 
 * It builds the curl command with credentials and the file path, then runs it using system().
 * 
 * If the upload fails, it logs a warning; if successful, it logs the time taken and the uploaded path.
 */
static void ftp_enqueue(const char *filename)
{
    pthread_mutex_lock(&ftp_q_lock);

    int next = (ftp_q_head + 1) % FTP_QUEUE_SIZE;
    if (next == ftp_q_tail) {
        warn("FTP queue full, dropping %s", filename);
    } else {
        ftp_queue[ftp_q_head] = strdup(filename);  
        ftp_q_head = next;
        sem_post(&sem_ftp);
    }

    pthread_mutex_unlock(&ftp_q_lock);
}




/*
 * This function gets system information using the uname() system call and stores it in a buffer.
 *
 * It fills uname_buf with details like OS name, hostname, release, version, and machine type.
 * 
 * This cached information can be reused later (e.g., in image file headers) without calling uname() again.
 */
static void cache_uname(void)
{
    struct utsname u;
    if (uname(&u) == 0)
        snprintf(uname_buf, sizeof uname_buf, "%s %s %s %s %s",
                 u.sysname, u.nodename, u.release, u.version, u.machine);
}



/*
 * This function opens the camera device file for reading and writing in non-blocking mode.
 *
 * It sets the global file descriptor cam_fd. If opening the device fails, the program exits with an error.
 */
static void cam_open()
{
    cam_fd = open(CAM_DEV, O_RDWR | O_NONBLOCK);
    if (cam_fd < 0) die("open %s", CAM_DEV);
}


/*
 * This function sets the camera format to capture video frames in YUYV format with a specific resolution.
 *
 * It uses the V4L2 API to apply the desired format. If the format setting fails or the device doesn't support YUYV,
 * the program exits with an error.
 */
static void cam_fmt()
{
    struct v4l2_format f = {0};
    f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    f.fmt.pix.width = W; f.fmt.pix.height = H; f.fmt.pix.pixelformat = PIX;
    if (ioctl(cam_fd, VIDIOC_S_FMT, &f) < 0) die("S_FMT");
    if (f.fmt.pix.pixelformat != PIX) die("device doesn't do YUYV");
}



/*
 * This function sets up memory-mapped buffers for the camera using the V4L2 API.
 *
 * It requests a set of buffers from the driver, maps them to user space using mmap(),
 * 
 * and queues them for capturing. If any step fails (requesting, querying, mapping, or queuing),
 * the program exits with an error.
 */
static void cam_bufs()
{
    struct v4l2_requestbuffers r = { .count = DRV_BUFS,
                                     .type  = V4L2_BUF_TYPE_VIDEO_CAPTURE,
                                     .memory= V4L2_MEMORY_MMAP };
    if (ioctl(cam_fd, VIDIOC_REQBUFS, &r) < 0) die("REQBUFS");
    for (unsigned i = 0; i < r.count; i++) {
        struct v4l2_buffer b = { .type=r.type, .memory=r.memory, .index=i };
        if (ioctl(cam_fd, VIDIOC_QUERYBUF, &b) < 0) die("QUERYBUF");
        drv[i].len = b.length;
        drv[i].start = mmap(NULL, b.length, PROT_READ|PROT_WRITE,
                            MAP_SHARED, cam_fd, b.m.offset);
        if (drv[i].start == MAP_FAILED) die("mmap");
        if (ioctl(cam_fd, VIDIOC_QBUF, &b) < 0) die("QBUF initial");
    }
}

/*
 * These two functions control the camera stream using the V4L2 API.
 *
 * cam_on():  Starts the video stream by calling VIDIOC_STREAMON.
 * cam_off(): Stops the video stream by calling VIDIOC_STREAMOFF.
 *
 * They tell the camera when to begin and end capturing frames.
 */
static void cam_on()  { enum v4l2_buf_type t=V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(cam_fd, VIDIOC_STREAMON, &t); }
static void cam_off() { enum v4l2_buf_type t=V4L2_BUF_TYPE_VIDEO_CAPTURE; ioctl(cam_fd, VIDIOC_STREAMOFF,&t); }




/*
 * This function converts a YUYV image to a grayscale image.
 *
 * y2g(): Extracts the Y (luminance) values from the YUYV format and stores them
 *        into a grayscale buffer `g`.
 *
 * It processes two pixels at a time and keeps only the brightness information.
 */
static inline void y2g(const uint8_t *yuyv, uint8_t *g)
{
    size_t n = (size_t)W * H / 2;
    for (size_t i = 0; i < n; i++) {
        g[2*i]   = yuyv[4*i];
        g[2*i+1] = yuyv[4*i+2];
    }
}

/*
 * This function applies Sobel edge detection to a grayscale image.
 *
 * sobel_edge(): Calculates the gradient in both X and Y directions using the Sobel operator,
 *               then computes the magnitude of the gradient to highlight edges in the image.
 *
 * The result is stored in the `dst` buffer, which contains the edge-detected version of `src`.
 */
void sobel_edge(uint8_t *src, uint8_t *dst)
{
    for (int y = 1; y < H - 1; y++) {
        for (int x = 1; x < W - 1; x++) {
            int gx = -src[(y-1)*W + (x-1)] - 2*src[y*W + (x-1)] - src[(y+1)*W + (x-1)]
                     + src[(y-1)*W + (x+1)] + 2*src[y*W + (x+1)] + src[(y+1)*W + (x+1)];

            int gy = -src[(y-1)*W + (x-1)] - 2*src[(y-1)*W + x] - src[(y-1)*W + (x+1)]
                     + src[(y+1)*W + (x-1)] + 2*src[(y+1)*W + x] + src[(y+1)*W + (x+1)];

            int mag = (int)sqrt(gx * gx + gy * gy);
            dst[y * W + x] = (mag > 255) ? 255 : mag;
        }
    }
}

/*
 * This function generates a timestamped filename for a saved image.
 *
 * ts_name(): Combines the frame index, current date and time, and the nanosecond part of a timestamp
 *            to create a unique filename in the format "f###_YYYYMMDD_HHMMSS_NNNNNNNNN.pgm".
 *
 * The result is stored in the `buf` string, which can be used to save the image to disk.
 */
static void ts_name(char *buf, size_t cap, int idx, struct timespec t)
{
    struct timespec rt; clock_gettime(CLOCK_REALTIME,&rt);
    struct tm tm; localtime_r(&rt.tv_sec,&tm);
    char date[32]; strftime(date,sizeof date,"%Y%m%d_%H%M%S",&tm);
    snprintf(buf,cap,"f%03d_%s_%09ld.pgm",idx,date,t.tv_nsec);
}


/*
 * This function saves a grayscale image to disk in PGM (Portable GrayMap) format.
 *
 * save_pgm(): Opens the file at the given path and writes a PGM header followed by the image data.
 *             It includes a timestamp and system information in the comment section of the header.
 *             If writing fails, it logs a warning message.
 *
 * The image is expected to be in 8-bit grayscale format, with width W and height H.
 */
static void save_pgm(const uint8_t *g, const char *path)
{
    int fd = open(path,O_CREAT|O_TRUNC|O_WRONLY,0644);
    if (fd<0){ warn("open %s",path); return; }
  /* timestamp for this frame */
    struct timespec rt;
    clock_gettime(CLOCK_REALTIME, &rt);
    struct tm tm;
    localtime_r(&rt.tv_sec, &tm);
    char ts[40];
    strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm);

    
    char hdr[512];
    int hl = snprintf(hdr, sizeof hdr,
                      "P5\n"
                      "# ts=%s.%09ld\n"
                      "# uname=%s\n"
                      "%d %d\n"
                      "255\n",
                      ts, rt.tv_nsec, uname_buf, W, H);
    
    if (w_all(fd,hdr,hl)<0 || w_all(fd,g,W*H)<0) warn("write %s",path);
    close(fd);
}



/*
 * th_cap(): This is the Capture Thread responsible for grabbing camera frames.
 *
 * - Sets the thread to real-time priority and pins it to the working CPU core.
 * 
 * - Waits for a signal from the Sequencer using a semaphore.
 * 
 * - Dequeues a frame from the camera driver using VIDIOC_DQBUF.
 * 
 * - Converts the YUYV image to grayscale and stores it in the ring buffer.
 * 
 * - Timestamps the frame and marks the slot as full.
 * 
 * - Re-queues the buffer for the next capture using VIDIOC_QBUF.
 * 
 * - Logs the time between captures (period) and time spent on capturing (execution time).
 * 
 * - Signals the Processing thread to begin its work.
 */
static void *th_cap(void *arg)
{
    set_rt(CAP_PRIO); 
    pin(WRK_CORE);
    struct timespec prev = {0}, t0, t1;

    while (1) {
        sem_wait(&sem_cap);
        clock_gettime(CLOCK_MONOTONIC, &t0);  

        
        struct v4l2_buffer b = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE,
            .memory = V4L2_MEMORY_MMAP
        };
        while (ioctl(cam_fd, VIDIOC_DQBUF, &b)) {
            if (errno == EAGAIN) {
                usleep(1000);
                continue;
            }
            die("DQBUF");
        }

       
        size_t slot = head & (RING_SIZE - 1);
        if (!ring[slot].gray)
            ring[slot].gray = malloc(W * H);

        y2g((uint8_t *)drv[b.index].start, ring[slot].gray);
        clock_gettime(CLOCK_MONOTONIC, &ring[slot].ts);
        ring[slot].full = true;
        head++;

        
        if (ioctl(cam_fd, VIDIOC_QBUF, &b) < 0)
            die("QBUF re");

        clock_gettime(CLOCK_MONOTONIC, &t1);  

        
        if (prev.tv_sec) {
            syslog(LOG_INFO, "CAP  period=%.3f ms  exec=%.3f ms",
                   ns_to_ms(ns_diff(&t0, &prev)),
                   ns_to_ms(ns_diff(&t1, &t0)));
        }
        prev = t0;

        sem_post(&sem_proc);
    }

    return NULL;
}


/*
 * th_proc(): Processing thread that applies blur and edge detection to camera frames.
 *
 * - Runs with real-time priority and is pinned to a specific CPU core.
 * 
 * - Waits for a signal from the capture thread (sem_proc).
 * 
 * - Applies a 3x3 box blur to the grayscale frame to reduce noise.
 * 
 * - Preserves the edge pixels by copying them from the original frame.
 * 
 * - Applies Sobel edge detection on the blurred image to highlight edges.
 * 
 * - Overwrites the original frame data with the edge-detected result.
 * 
 * - Marks the frame as full and ready to be written to disk.
 * 
 * - Logs execution time and frame processing statistics using syslog.
 * 
 * - Signals the write thread to handle the processed frame.
 */
static void *th_proc(void *arg)
{
    set_rt(PROC_PRIO);
    pin(WRK_CORE);
    struct timespec prev = {0}, t0, t1;

    static uint8_t blurred[W * H];
    static uint8_t edge[W * H];
    int frame_count = 0;

    while (1) {
        sem_wait(&sem_proc);
        clock_gettime(CLOCK_MONOTONIC, &t0);

        size_t slot = tail & (RING_SIZE - 1);
        uint8_t *gray = ring[slot].gray;
        if (!gray) {
            sem_post(&sem_wr);
            continue;
        }

       
        for (int y = 1; y < H - 1; y++) {
            for (int x = 1; x < W - 1; x++) {
                int sum = 0;
                for (int dy = -1; dy <= 1; dy++) {
                    for (int dx = -1; dx <= 1; dx++) {
                        sum += gray[(y + dy) * W + (x + dx)];
                    }
                }
                blurred[y * W + x] = sum / 9;
            }
        }

       
        for (int x = 0; x < W; x++) {
            blurred[0 * W + x] = gray[0 * W + x];
            blurred[(H - 1) * W + x] = gray[(H - 1) * W + x];
        }
        for (int y = 0; y < H; y++) {
            blurred[y * W + 0] = gray[y * W + 0];
            blurred[y * W + (W - 1)] = gray[y * W + (W - 1)];
        }

        
        sobel_edge(blurred, edge);

       
        memcpy(ring[slot].gray, edge, W * H);
        ring[slot].full = true;

        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (prev.tv_sec) {
            syslog(LOG_INFO, "PROC period=%.3f ms  exec=%.3f ms",
                   ns_to_ms(ns_diff(&t0, &prev)),
                   ns_to_ms(ns_diff(&t1, &t0)));
        }
        prev = t0;

        frame_count++;
        sem_post(&sem_wr);
    }

    return NULL;
}




/*
 * th_wr(): Write thread that saves grayscale frames to disk and queues them for FTP upload.
 *
 * - Runs with real-time priority and is pinned to a specific CPU core.
 * 
 * - Waits for a signal from the processing thread (sem_wr).
 * 
 * - Retrieves the next full frame from the ring buffer.
 * 
 * - Generates a timestamped filename and saves the frame as a PGM image.
 * 
 * - Every FTP_STRIDE frames, adds the image path to the FTP upload queue.
 * 
 * - Logs frame saving and performance stats using syslog.
 * 
 * - Frees the ring buffer slot and increments the tail index.
 * 
 * - Stops execution once MAX_FRAMES is reached.
 */
static int fidx = 0;
static void *th_wr(void *arg)
{
    set_rt(WR_PRIO);
    pin(WRK_CORE);
    struct timespec prev = {0}, t0, t1;

    while (1) {
        sem_wait(&sem_wr);
        clock_gettime(CLOCK_MONOTONIC, &t0);

        size_t slot = tail & (RING_SIZE - 1);
        if (!ring[slot].full) continue;

        char name[96];
        ts_name(name, sizeof name, fidx++, ring[slot].ts);
        save_pgm(ring[slot].gray, name);

        


        if ((fidx - 1) % FTP_STRIDE == 0) {
                ftp_enqueue(name);
        }


        syslog(LOG_INFO, "Saved frame %d as %s", fidx - 1, name);

        ring[slot].full = false;
        tail++;

        if (fidx >= MAX_FRAMES) exit(0);

        clock_gettime(CLOCK_MONOTONIC, &t1);
        if (prev.tv_sec) {
            syslog(LOG_INFO, "WR   period=%.3f ms  exec=%.3f ms",
                   ns_to_ms(ns_diff(&t0, &prev)),
                   ns_to_ms(ns_diff(&t1, &t0)));
        }
        prev = t0;
    }
    return NULL;
}


/*
 * th_ftp(): This is the FTP Uploader Thread responsible for uploading images.
 *
 * - Pins the thread to the FTP-designated core.
 * 
 * - Waits for a signal via a semaphore before checking the queue.
 * 
 * - Uses a mutex lock to safely access the FTP upload queue.
 * 
 * - If the queue has a path to upload, it retrieves the file path,
 * 
 *   uploads the file using ftp_upload(), and then frees the memory.
 * 
 * - Repeats the loop to handle the next upload request.
 */
static void *th_ftp(void *arg)
{
    pin(FTP_CORE);  
    while (1) {
        sem_wait(&sem_ftp);

        pthread_mutex_lock(&ftp_q_lock);
        if (ftp_q_tail != ftp_q_head) {
            char *path = ftp_queue[ftp_q_tail];
            ftp_q_tail = (ftp_q_tail + 1) % FTP_QUEUE_SIZE;
            pthread_mutex_unlock(&ftp_q_lock);

            ftp_upload(path);
            free(path);
        } else {
            pthread_mutex_unlock(&ftp_q_lock);
        }
    }
    return NULL;
}


/*
 * th_seq(): This is the Sequencer Thread that drives the timing of the system.
 *
 * - Sets real-time priority and pins the thread to a specific core.
 * 
 * - Calculates the next activation time using a fixed interval (PERIOD_NS).
 * 
 * - Sleeps until that exact time using clock_nanosleep().
 * 
 * - Posts to the capture semaphore to trigger the Capture Thread.
 * 
 * - Logs the timing to monitor how regularly and quickly the sequencer runs.
 * 
 * - This thread ensures that image capture happens exactly every 1s or 0.1s.
 */

static void *th_seq(void *arg)
{
    set_rt(SEQ_PRIO);
    pin(SEQ_CORE);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    struct timespec prev = {0}, t0, t1;

    while (1) {
        add_ns(&next, PERIOD_NS);
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        clock_gettime(CLOCK_MONOTONIC, &t0);  

        sem_post(&sem_cap);                   

        clock_gettime(CLOCK_MONOTONIC, &t1);  

        if (prev.tv_sec) {
            syslog(LOG_INFO, "SEQ  period=%.3f ms  exec=%.3f ms",
                   ns_to_ms(ns_diff(&t0, &prev)),
                   ns_to_ms(ns_diff(&t1, &t0)));
        }
        prev = t0;
    }
}


/*
 *
 * - Caches system info and initializes system logging.
 * 
 * - Opens and configures the camera (format, buffers, and streaming).
 * 
 * - Initializes semaphores for thread synchronization.
 * 
 * - Creates five threads: sequencer, capture, processing, writing, and FTP upload.
 * 
 * - Waits for all threads to finish (they usually run indefinitely).
 * 
 * - Cleans up by turning off the camera and closing system logs.
 */

int main(void)
{
    cache_uname();
    openlog("realtime-vision", LOG_PID|LOG_CONS, LOG_USER);
    syslog(LOG_INFO, "Start 1 Hz 4-thread vision");

    cam_open(); 
    cam_fmt(); 
    cam_bufs();
    cam_on();

    sem_init(&sem_cap,0,0);
    sem_init(&sem_proc,0,0);
    sem_init(&sem_wr,0,0);

    pthread_t thSeq, thCap, thProc, thW;
    pthread_create(&thSeq, 0, th_seq,  0);
    pthread_create(&thCap, 0, th_cap,  0);
    pthread_create(&thProc,0, th_proc, 0);
    pthread_create(&thW,   0, th_wr,   0);

   
    sem_init(&sem_ftp, 0, 0);
    pthread_t thFtp;
    pthread_create(&thFtp, 0, th_ftp, 0);


    pthread_join(thSeq, NULL);
    pthread_join(thCap, NULL);
    pthread_join(thProc,NULL);
    pthread_join(thW,   NULL);
    pthread_join(thFtp, NULL);

    cam_off(); close(cam_fd);
    syslog(LOG_INFO, "Done"); closelog();
    return 0;
}
