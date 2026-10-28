/*
HOST_OMNI.C

Virtuix Omni One treadmill, for the VR build. Omni Connect (the treadmill's
Android app) shares a 320-byte region with the game (OmniOne.java passes
the descriptor in). The layout below is what its libomnishm.so reads and
writes; the first part is Omni Connect's, the part from 0x20 the game's.

	0x00 u32   flags: bit 0 data written, bit 1 treadmill connected
	0x04 f32   movement x (right) and 0x08 y (forward), -1 to 1
	0x0C f32   the ring's yaw
	0x10 i32   steps this session
	0x20 u8    bit 0 a game is running, bit 1 camera yaw given, bit 2 advanced data asked for
	0x24 f32   camera (head) yaw, degrees
	0x30 u64   the game's heartbeat, milliseconds on the boot clock
	0x60 char[64] the game's name, 0xA0 char[64] its engine, 0xE0 char[32] the headset,
	           0x100 char[32] the SDK's version

The movement is the treadmill's, turned against the head yaw the game gives.
*/

#include "host.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_system.h>
#include <jni.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define OMNI_REGION_SIZE 0x140

#define OMNI_FLAG_CONNECTED 0x2u
#define OMNI_SDK_ALIVE 0x1
#define OMNI_SDK_CAMERA_YAW 0x2

static pthread_mutex_t omni_lock = PTHREAD_MUTEX_INITIALIZER;
static volatile uint8_t *omni_region;
static int omni_wanted, omni_was_connected;
static uint64_t omni_last_log;

/* asks the activity (omniStart, omniStop: HaloActivity), whose class loader finds OmniOne */
static void omni_activity_call(const char *method)
{
	JNIEnv *env = (JNIEnv *)SDL_GetAndroidJNIEnv();
	jobject activity = env ? (jobject)SDL_GetAndroidActivity() : NULL;
	jclass type;
	jmethodID id;

	if (!activity)
		return;
	type = (*env)->GetObjectClass(env, activity);
	id = type ? (*env)->GetMethodID(env, type, method, "()V") : NULL;
	if (id)
		(*env)->CallVoidMethod(env, activity, id);
	if ((*env)->ExceptionCheck(env))
	{
		(*env)->ExceptionClear(env);
		host_logf(HOST_LOG_WARN, "[omni] %s failed", method);
	}
	if (type)
		(*env)->DeleteLocalRef(env, type);
	(*env)->DeleteLocalRef(env, activity);
}

static void omni_unmap(void)
{
	if (omni_region)
	{
		munmap((void *)omni_region, OMNI_REGION_SIZE);
		omni_region = NULL;
	}
	omni_was_connected = 0;
}

JNIEXPORT void JNICALL Java_com_halo_decomp_OmniOne_nativeAttach(JNIEnv *env, jclass type, jint descriptor)
{
	void *region;

	(void)env; (void)type;
	region = mmap(NULL, OMNI_REGION_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, descriptor, 0);
	close(descriptor);
	pthread_mutex_lock(&omni_lock);
	omni_unmap();
	if (region == MAP_FAILED)
		host_logf(HOST_LOG_WARN, "[omni] mapping Omni Connect's shared memory failed");
	else
	{
		omni_region = (volatile uint8_t *)region;
		host_logf(HOST_LOG_INFO, "[omni] Omni Connect's shared memory mapped");
	}
	pthread_mutex_unlock(&omni_lock);
}

JNIEXPORT void JNICALL Java_com_halo_decomp_OmniOne_nativeDetach(JNIEnv *env, jclass type)
{
	(void)env; (void)type;
	pthread_mutex_lock(&omni_lock);
	omni_unmap();
	pthread_mutex_unlock(&omni_lock);
}

static void omni_text(volatile uint8_t *region, unsigned offset, unsigned size, const char *text)
{
	char buffer[64] = { 0 };

	strncpy(buffer, text, size - 1);
	memcpy((void *)(region + offset), buffer, size);
}

/*
Once a frame. enable: OMNI ONE is on (binds to Omni Connect when it first is, lets go when it is not).
head_yaw_degrees: the head's yaw as Omni Connect wants it. out gets the movement x (right), y (forward)
and the ring's yaw. Returns bit 0 when Omni Connect's region is attached, bit 1 when the treadmill is
connected, else 0.
*/
int host_omni_poll(int enable, float head_yaw_degrees, float *out)
{
	volatile uint8_t *region;
	int result = 0;

	out[0] = out[1] = out[2] = 0.0f;
	if (enable != omni_wanted)
	{
		omni_wanted = enable;
		host_logf(HOST_LOG_INFO, "[omni] Omni One %s", enable ? "on: binding to Omni Connect" : "off");
		omni_activity_call(enable ? "omniStart" : "omniStop");
	}
	if (!enable)
		return 0;
	pthread_mutex_lock(&omni_lock);
	region = omni_region;
	if (region)
	{
		struct timespec now;
		uint64_t milliseconds;
		uint32_t flags;

		/* the names first, then the heartbeat and the flags that say the game is there */
		if (!region[0x60])
		{
			omni_text(region, 0x60, 64, "Halo CE");
			omni_text(region, 0xA0, 64, "HaloCE-DecompRemix");
			omni_text(region, 0xE0, 32, "Meta Quest");
			omni_text(region, 0x100, 32, "halo-omni-1");
		}
		clock_gettime(CLOCK_BOOTTIME, &now);
		milliseconds = (uint64_t)now.tv_sec * 1000u + (uint64_t)(now.tv_nsec / 1000000);
		memcpy((void *)(region + 0x24), &head_yaw_degrees, sizeof(float));
		memcpy((void *)(region + 0x30), &milliseconds, sizeof(milliseconds));
		atomic_thread_fence(memory_order_release);
		region[0x20] = OMNI_SDK_ALIVE | OMNI_SDK_CAMERA_YAW;

		atomic_thread_fence(memory_order_acquire);
		memcpy(&flags, (const void *)region, sizeof(flags));
		result = 1;
		if (flags & OMNI_FLAG_CONNECTED)
		{
			result |= 2;
			memcpy(out, (const void *)(region + 4), 3 * sizeof(float));
			/* while it moves, once a second: what was given and what came back (to tune the yaw's sign) */
			if ((out[0] != 0.0f || out[1] != 0.0f) && milliseconds - omni_last_log >= 1000)
			{
				omni_last_log = milliseconds;
				host_logf(HOST_LOG_INFO, "[omni] head yaw given %.1f, ring yaw %.1f, movement x %.2f y %.2f",
					head_yaw_degrees, out[2], out[0], out[1]);
			}
		}
		if (!!(flags & OMNI_FLAG_CONNECTED) != omni_was_connected)
		{
			omni_was_connected = !!(flags & OMNI_FLAG_CONNECTED);
			host_logf(HOST_LOG_INFO, "[omni] the treadmill is %s (flags %08x)",
				omni_was_connected ? "connected" : "not connected", (unsigned)flags);
		}
	}
	pthread_mutex_unlock(&omni_lock);
	return result;
}
