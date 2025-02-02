/// \file JamUtil.c
/// \author Paolo Mazzon
#include <stdio.h>
#include <stdarg.h>
#include <VK2D/stb_image.h>
#include <SDL2/SDL_syswm.h>
//#include <pthread.h>

#include "cute_sound.h"
#include "JamUtil.h"

/********************** Constants **********************/
const uint32_t JU_BUCKET_SIZE = 100;            // A good size for a small jam game, feel free to adjust
const uint32_t JU_BINARY_FONT_HEADER_SIZE = 13; // Size of the header of jufnt files
const uint32_t JU_STRING_BUFFER = 1024;         // Maximum amount of text that can be rendered at once, a kilobyte is good for most things
const uint32_t JU_SAVE_MAX_SIZE = 2000;         // Maximum pieces of data that can be loaded from a save, anything more than this is probably a corrupt file
const uint32_t JU_SAVE_MAX_KEY_SIZE = 20;       // Maximum size a save key can be
const int JU_LIST_EXTENSION = 5;                // How many elements to extend lists by
const JUEntityID JU_INVALID_ENTITY = -1;
const JUComponentID JU_NO_COMPONENT = -1;
const int JU_JOB_CHANNEL_SYSTEMS = 0;
const int JU_JOB_CHANNEL_COPY = 1;
const int32_t JU_DISABLED_LOCK = -1;
const JUEntityType JU_INVALID_TYPE = 0;

#if SDL_BYTEORDER == SDL_BIG_ENDIAN
uint32_t RMASK = 0xff000000;
uint32_t GMASK = 0x00ff0000;
uint32_t BMASK = 0x0000ff00;
uint32_t AMASK = 0x000000ff;
#else // little endian, like x86
uint32_t RMASK = 0x000000ff;
uint32_t GMASK = 0x0000ff00;
uint32_t BMASK = 0x00ff0000;
uint32_t AMASK = 0xff000000;
#endif

/********************** "Private" Structs **********************/

/// \brief Character dimensions in the jufnt file
typedef struct JUBinaryCharacter {
	uint16_t width;  ///< Width of the character
	uint16_t height; ///< Height of the character
} JUBinaryCharacter;

/// \brief This is an unpacked representation of a binary jufnt file
typedef struct JUBinaryFont {
	uint32_t size;                          ///< Size in bytes of the png
	uint32_t characters;                    ///< Total number of characters in the font
	JUBinaryCharacter *characterDimensions; ///< Vector of jufnt characters
	void *png;                              ///< Raw bytes for the png image
} JUBinaryFont;

/// \brief Information for jobs
typedef struct JUJobSystem {
	int threadCount;             ///< Number of worker threads being used
	//pthread_t *threads;          ///< Thread vector
	int queueListSize;           ///< Actual size of the queue vector
	int queueSize;               ///< Number of elements waiting in the queue
	JUJob *queue;                ///< Queue (vector)
	//pthread_mutex_t queueAccess; ///< Mutex that protects access to the queue
	//_Atomic int *channels;       ///< Variable number of channels
	int channelCount;            ///< Number of available channels
	//_Atomic bool kill;           ///< For shutting down all jobs
} JUJobSystem;

/// \brief Information for ECS
typedef struct JUECS {
	JUEntity *entities;                    ///< Vector of all entities
	int entityCount;                       ///< Number of entities
	JUSystem *systems;               ///< List of all systems
	int systemCount;                       ///< Amount of systems
	//_Atomic bool *systemFinished;          ///< Whether or not each system is done executing this frame
	JUComponentVector* previousComponents; ///< Previous frame's components
	JUComponentVector *components;         ///< This frame's components
	const int componentCount;              ///< Amount of components
	const size_t *componentSizes;          ///< Size of each component in bytes
	int *componentListSizes;               ///< Actual size of component list
	//pthread_mutex_t createEntityAccess;    ///< Lock so only 1 entity may be created at a time
	int entityIterator;                    ///< Basically the i value for the entity iterating functions
} JUECS;

/********************** Globals **********************/
static cs_context_t *gSoundContext = NULL;               // For the audio player
int gKeyboardSize = 0;                                   // For keeping track of keys through SDL
static uint8_t *gKeyboardState, *gKeyboardPreviousState; // Arrays for key states through SDL
static double gDelta = 0;                                // Delta time
static uint64_t gLastTime = 0;                           // For keeping track of delta
static uint64_t gProgramStartTime = 0;                   // Time when the program started
static JUJobSystem gJobSystem;                           // Information for the job system
static JUECS gECS;                                       // Entity component system
static uint32_t gStringBuffer[1000];                     // For UTF-8 decoding
static int gStringBufferSize = 1000;                     // For UTF-8 decoding
static  vec4 gColours[7];

/********************** Static Functions **********************/

// Logs messages, used all over the place
static void juLog(const char *out, ...) {
	va_list list;
	va_start(list, out);
	printf("[JamUtil] ");
	vprintf(out, list);
	printf("\n");
	va_end(list);
	fflush(stdout);
}

// Allocates memory, crashing if it doesn't work
static void *juMalloc(uint32_t size) {
	void *out = malloc(size);
	if (out == NULL) {
		juLog("Failed to allocate memory");
		abort();
	}
	return out;
}

// Allocates memory, crashing if it doesn't work
static void *juMallocZero(uint32_t size) {
	void *out = calloc(1, size);
	if (out == NULL) {
		juLog("Failed to allocate memory");
		abort();
	}
	return out;
}

// Reallocates memory, crashing if it doesn't work
static void *juRealloc(void *ptr, uint32_t newSize) {
	void *newptr = realloc(ptr, newSize);
	if (newptr == NULL) {
		juLog("Failed to allocate memory");
		abort();
	}
	return newptr;
}

// Frees
static void juFree(void *ptr) {
	free(ptr);
}

// Hashes a string into a 32 bit number between 0 and JU_BUCKET_SIZE
static uint32_t juHash(const char *string) {
	uint32_t hash = 5381;
	int i = 0;

	while (string[i] != 0) {
		hash = ((hash << 5) + hash) + string[i]; /* hash * 33 + c */
		i++;
	}

	return hash % JU_BUCKET_SIZE;
}

// Find the position of the last period in a string (for filenames)
static uint32_t juLastDot(const char *string) {
	uint32_t dot = 0;
	uint32_t pos = 0;
	while (string[pos] != 0) {
		if (string[pos] == '.') dot = pos;
		pos++;
	}

	return dot;
}

// Copies a string
static const char *juCopyString(const char *string) {
	char *out = juMalloc(strlen(string) + 1);
	strcpy(out, string);
	return out;
}

// Swaps bytes between little and big endian
/*static void juSwapEndian(void* bytes, uint32_t size) {
	uint8_t new[size];
	memcpy(new, bytes, size);
	for (int i = size - 1; i >= 0; i--)
		((uint8_t*)bytes)[i] = new[size - i - 1];
}*/

static void juCopyFromBigEndian(void *dst, void *src, uint32_t size) {
	memcpy(dst, src, size);
#if SDL_BYTEORDER == SDL_LIL_ENDIAN
	juSwapEndian(dst, size);
#endif // SDL_LIL_ENDIAN
}

// Dumps a file into a binary buffer (free it yourself)
static uint8_t *juGetFile(const char *filename, uint32_t *size) {
	uint8_t *buffer = NULL;
	FILE *file = fopen(filename, "rb");
	int len = 0;

	if (file != NULL) {
		while (!feof(file)) {
			fgetc(file);
			len++;
		}
		//len--;
		*size = len;
		buffer = juMalloc(len);
		rewind(file);
		fread(buffer, len, 1, file);

		fclose(file);
	} else {
		juLog("Couldn't open file \"%s\"", filename);
	}

	return buffer;
}

// Loads all jufnt data into a struct
static JUBinaryFont juLoadBinaryFont(const char *file, bool *error) {
	JUBinaryFont font = {0};
	*error = false;
	uint32_t size;
	uint8_t *buffer = juGetFile(file, &size);
	uint32_t pointer = 5; // We don't care about the header

	if (buffer != NULL && size >= JU_BINARY_FONT_HEADER_SIZE) {
		// png size
		juCopyFromBigEndian(&font.size, buffer + pointer, 4);
		pointer += 4;

		// number of characters
		juCopyFromBigEndian(&font.characters, buffer + pointer, 4);
		pointer += 4;

		// We now have enough data to calculate the total size the file should be
		if (size - 1 == 13 + font.size + (font.characters * 4)) {
			font.size--;
			font.characterDimensions = juMalloc(font.characters * sizeof(struct JUBinaryCharacter));
			font.png = juMalloc(font.size);

			// Grab all the characters
			for (int i = 0; i < font.characters; i++) {
				juCopyFromBigEndian(&font.characterDimensions[i].width, buffer + pointer, 2);
				pointer += 2;
				juCopyFromBigEndian(&font.characterDimensions[i].height, buffer + pointer, 2);
				pointer += 2;
			}

			memcpy(font.png, buffer + pointer, font.size);

		} else {
			juLog("jufnt file \"%s\" is unreadable", file);
		}
	} else {
		*error = true;
	}
	free(buffer);

	return font;
}

// Worker thread
/*
static void *juWorkerThread(void *data) {
	bool haveJob;
	JUJob job;

	while (!gJobSystem.kill) {
		// Wait for a job
		haveJob = false;
		pthread_mutex_lock(&gJobSystem.queueAccess);
		if (gJobSystem.queueSize > 0) {
			job = gJobSystem.queue[0];
			haveJob = true;
			for (int i = 0; i < gJobSystem.queueSize - 1; i++)
				gJobSystem.queue[i] = gJobSystem.queue[i + 1];
			gJobSystem.queueSize--;
		}
		pthread_mutex_unlock(&gJobSystem.queueAccess);

		// Execute the job
		if (haveJob) {
			job.job(job.data);
			gJobSystem.channels[job.channel] -= 1;
		}
	}

	return NULL;
}*/

#define UTF8_IS_1_BYTE(b) ((b & 0b10000000) == 0b00000000)
#define UTF8_IS_2_BYTE(b) ((b & 0b11100000) == 0b11000000)
#define UTF8_IS_3_BYTE(b) ((b & 0b11110000) == 0b11100000)
#define UTF8_IS_4_BYTE(b) ((b & 0b11111000) == 0b11110000)
#define UTF8_IS_X_BYTE(b) ((b & 0b11000000) == 0b10000000)

// Returns the size of the array and fills a given uint32_t array up to size
static int utf8Decode(const char *str, uint32_t *codePoints, int size) {
	if (str != NULL) {
		int len = 0;
		bool invalid = false;
		int expectedXBytes = 0;
		int arraySlot = 0;
		uint32_t character = 0;
		for (int i = 0; str[i] != 0 && !invalid && arraySlot < size; i++) {
			if (expectedXBytes == 0) {
				if (UTF8_IS_1_BYTE(str[i])) {
					codePoints[arraySlot] = str[i] & 0b01111111;
					arraySlot++;
					len += 1;
				} else if (UTF8_IS_2_BYTE(str[i])) {
					character = str[i] & 0b00011111;
					expectedXBytes = 2 - 1;
				} else if (UTF8_IS_3_BYTE(str[i])) {
					character = str[i] & 0b00001111;
					expectedXBytes = 3 - 1;
				} else if (UTF8_IS_4_BYTE(str[i])) {
					character = str[i] & 0b00000111;
					expectedXBytes = 4 - 1;
				} else {
					invalid = true;
				}
			} else {
				if (UTF8_IS_X_BYTE(str[i])) {
					expectedXBytes--;
					character = (character << 6) + (str[i] & 0b00111111);
					if (expectedXBytes == 0) {
						codePoints[arraySlot] = character;
						arraySlot++;
						character = 0;
						len += 1;
					}
				} else {
					invalid = true;
				}
			}
		}

		return expectedXBytes == 0 && !invalid ? len : 0;
	}
	return 0;
}

/********************** Top-Level **********************/

void juInit(SDL_Window *window, int jobChannels, int minimumThreads) {
	// Sound
	SDL_SysWMinfo wmInfo;
	SDL_VERSION(&wmInfo.version)
	//SDL_GetWindowWMInfo(window, &wmInfo);
	//HWND hwnd = wmInfo.info.win.window;
	gSoundContext = cs_make_context(NULL, 44100, 1024 * 1024 * 10, 100, NULL);
	if (gSoundContext != NULL) {
		cs_spawn_mix_thread(gSoundContext);
	} else {
		juLog("Failed to initialize sound.");
	}

	// Keyboard controls
	gKeyboardState = (void*)SDL_GetKeyboardState(&gKeyboardSize);
	gKeyboardPreviousState = juMallocZero(gKeyboardSize);

	// Setup job system if any channels were specified
	/*if (jobChannels > 0) {
		gJobSystem.channelCount = jobChannels;
		if (minimumThreads == 0)
			gJobSystem.threadCount = SDL_GetCPUCount() - 1;
		else
			gJobSystem.threadCount = SDL_GetCPUCount() - 1 < minimumThreads ? minimumThreads : SDL_GetCPUCount() - 1;
		gJobSystem.channels = juMallocZero(jobChannels * sizeof(_Atomic int));
		gJobSystem.threads = juMalloc(gJobSystem.threadCount * sizeof(pthread_t));

		// Create worker threads
		for (int i = 0; i < gJobSystem.threadCount; i++) {
			pthread_attr_t attr;
			pthread_attr_init(&attr);
			pthread_create(&gJobSystem.threads[i], &attr, juWorkerThread, NULL);
		}

		// Setup the mutexes
		pthread_mutexattr_t attr;
		pthread_mutexattr_init(&attr);
		pthread_mutex_init(&gJobSystem.queueAccess, &attr);
		pthread_mutexattr_init(&attr);
		pthread_mutex_init(&gECS.createEntityAccess, &attr);
	}
	*/

	// Delta and other timing
	gLastTime = SDL_GetPerformanceCounter();
	gDelta = 1;
	gProgramStartTime = SDL_GetPerformanceCounter();

	vk2dColourHex(gColours[0], "#d9453b");
	vk2dColourHex(gColours[1], "#a18403");
	vk2dColourHex(gColours[2], "#edf051");
	vk2dColourHex(gColours[3], "#40cc3d");
	vk2dColourHex(gColours[4], "#3eaba9");
	vk2dColourHex(gColours[5], "#19248a");
	vk2dColourHex(gColours[6], "#8a196d");
}

void juUpdate() {
	// Update delta
	gDelta = (double)(SDL_GetPerformanceCounter() - gLastTime) / (double)SDL_GetPerformanceFrequency();
	gLastTime = SDL_GetPerformanceCounter();

	// Update keyboard
	memcpy(gKeyboardPreviousState, gKeyboardState, gKeyboardSize);
	SDL_PumpEvents();
}

void juQuit() {
	// Kill the jobs
	if (gJobSystem.channelCount > 0) {
		// Destroy ECS
		for (int i = 0; i < gECS.componentCount; i++) {
			juFree(gECS.components[i]);
			juFree(gECS.previousComponents[i]);
		}
		for (int i = 0; i < gECS.entityCount; i++) {
			juFree(gECS.entities[i].components);
		}
		juFree(gECS.entities);
		juFree(gECS.previousComponents);
		juFree(gECS.components);
		//juFree(gECS.systemFinished);

		// Destroy job system
		//gJobSystem.kill = true;

		// Wait for all threads to die
		//for (int i = 0; i < gJobSystem.threadCount; i++)
			//pthread_join(gJobSystem.threads[i], NULL);

		// Free the lists
		//juFree(gJobSystem.threads);
		//juFree(gJobSystem.channels);
		juFree(gJobSystem.queue);
		//pthread_mutex_destroy(&gJobSystem.queueAccess);
	}

	free(gKeyboardPreviousState);
	gKeyboardPreviousState = NULL;
	gKeyboardState = NULL;
	gKeyboardSize = 0;
	cs_shutdown_context(gSoundContext);
	gSoundContext = NULL;
}

double juDelta() {
	return gDelta;
}

double juTime() {
	return (double)(SDL_GetPerformanceCounter() - gProgramStartTime) / (double)SDL_GetPerformanceFrequency();
}

/********************** Clock **********************/

void juClockReset(JUClock *clock) {
	clock->totalTime = 0;
	clock->totalIterations = 0;
	clock->lastTime = 0;
	juClockStart(clock);
}

void juClockStart(JUClock *clock) {
	clock->lastTime = SDL_GetPerformanceCounter();
}

double juClockTime(JUClock *clock) {
	return ((double)SDL_GetPerformanceCounter() - (double)clock->lastTime) / (double)SDL_GetPerformanceFrequency();
}

double juClockTick(JUClock *clock) {
	double time = juClockTime(clock);
	clock->totalTime += time;
	clock->totalIterations++;
	juClockStart(clock);
	return time;
}

void juClockFramerate(JUClock *clock, double framerate) {
	double time = juClockTime(clock);
	clock->totalTime += time;
	clock->totalIterations++;

	bool done = false;
	while (!done)
		done = juClockTime(clock) >= 1.0 / framerate;

	juClockStart(clock);
}

double juClockGetAverage(JUClock *clock) {
	return clock->totalTime / clock->totalIterations;
}

/********************** Font **********************/

// modifiers for text
//  + [#FF12E1] - colour
//  + [15, -15] - x/y displacement
//  + [~2] - wavy text followed by speed
//  + [!2] - shaky text followed by max displacement
//  + [*] - rainbow text
//  + [] - clear all modifiers
//  + #[...] - the # signifies that this should not be treated as a modifier
//  and the pound will not be displayed

// Parses a modifier token, the given string must start at the first bracket
// and an integer representing how many characters the whole token takes up in
// that string is returned. The value in the parsed token is returned in one
// of the parameters, which is unidentified so just pass all the values and it
// will only modify the one its supposed to.
static const int TOKEN_INVALID = -1;
static const int TOKEN_CLEAR = 0;
static const int TOKEN_COLOUR = 1;
static const int TOKEN_DISPLACEMENT = 2;
static const int TOKEN_WAVY = 3;
static const int TOKEN_SHAKY = 4;
static const int TOKEN_RAINBOW = 5;
static int _juFontParseModifierToken(uint32_t *str, vec4 colour, float *x, float *y, float *wave, float *shake, bool *rainbow) {
	int len = 0;
	static char token[100] = {0};

	// Find the size of the token
	for (int i = 0; str[i] != 0 && len == 0; i++)
		if (str[i] == ']')
			len = i;

	// Its a valid token maybe
	if (len != 0 && len != 1) {
		str += 1;
		int tokenType = TOKEN_INVALID;
		bool nonSpace = false;
		int tokenLen = 0;

		// Decide the type while making a new string ignoring whitespace
		for (int i = 0; i < len - 1 && i < 99; i++) {
			if (isspace(str[i])) continue;
			if (isdigit(str[i]) && !nonSpace) tokenType = TOKEN_DISPLACEMENT;
			if (str[i] == '#' && !nonSpace) tokenType = TOKEN_COLOUR;
			if (str[i] == '*' && !nonSpace) tokenType = TOKEN_RAINBOW;
			if (str[i] == '~' && !nonSpace) tokenType = TOKEN_WAVY;
			if (str[i] == '!' && !nonSpace) tokenType = TOKEN_SHAKY;
			if (!nonSpace) nonSpace = true;
			token[tokenLen] = str[i];
			tokenLen++;
		}
		token[tokenLen] = 0;

		// The string token is now a no-whitespace token of type tokenType
		if (tokenType == TOKEN_DISPLACEMENT) {
			char *comma = strchr(token, ',');
			if (comma != NULL) {
				*comma = 0;
				comma += 1;
				*x = strtof(token, NULL);
				*y = strtof(comma, NULL);
			}
		} else if (tokenType == TOKEN_COLOUR) {
			if (tokenLen == 7)
				vk2dColourHex(colour, token);
		} else if (tokenType == TOKEN_RAINBOW) {
			*rainbow = true;
		} else if (tokenType == TOKEN_WAVY) {
			*wave = strtof(token + 1, NULL);
		} else if (tokenType == TOKEN_SHAKY) {
			*shake = strtof(token + 1, NULL);
		}
	} else if (len == 1) {
		// Clear command
		vk2dRendererGetColourMod(colour);
		*x = 0;
		*y = 0;
		*wave = 0;
		*shake = 0;
		*rainbow = false;
	}

	return len + 1;
}

void juFontUTF8Size(JUFont font, float *w, float *h, float width, const char *fmt, ...) {
	// Var args stuff
	unsigned char buffer[1024];
	va_list va;
	va_start(va, fmt);
	vsprintf((void*)buffer, fmt, va);
	va_end(va);

	// Information needed to draw the text
	float x = 0;
	*w = 0;
	*h = font->newLineHeight;
	float startX = x;
	int len = utf8Decode((void*)buffer, gStringBuffer, gStringBufferSize);

	// Loop through each character and render individually
	for (int i = 0; i < len; i++) {
		if (font->unicodeStart <= gStringBuffer[i] && font->unicodeEnd > gStringBuffer[i]) {
			JUCharacter *c = &font->characters[gStringBuffer[i] - font->unicodeStart];

			// Move to the next line if we're about to go over
			if ((width > 0 && (c->w + x) - startX > width) || gStringBuffer[i] == '\n') {
				if (x > *w) *w = x;
				x = startX;
				*h += font->newLineHeight;
			}
			if (gStringBuffer[i] != '\n') x += c->w;
		}
	}
	if (x > *w) *w = x;
}

void juFontUTF8SizeExt(JUFont font, float *w, float *h, float width, const char *string) {
	// Information needed to draw the text
	float x = 0;
	*w = 0;
	*h = font->newLineHeight;
	float startX = x;
	int len = utf8Decode((void*)string, gStringBuffer, gStringBufferSize);
	vec4 colour;
	float displacementX, displacementY, wave, shake;
	bool rainbow;

	// Loop through each character and render individually
	for (int i = 0; i < len; i++) {
		if (font->unicodeStart <= gStringBuffer[i] && font->unicodeEnd > gStringBuffer[i]) {
			// String tokens are parsed first
			if (gStringBuffer[i] == '[' && (i == 0 || gStringBuffer[i - 1] != '#')) {
				i += _juFontParseModifierToken(&gStringBuffer[i], colour, &displacementX, &displacementY, &wave, &shake, &rainbow) - 1;
				continue;
			} else if (gStringBuffer[i] == '#' && i < len - 1 && gStringBuffer[i + 1] == '[') {
				continue;
			}

			JUCharacter *c = &font->characters[gStringBuffer[i] - font->unicodeStart];

			// Move to the next line if we're about to go over
			if ((width > 0 && (c->w + x) - startX > width) || gStringBuffer[i] == '\n') {
				if (x > *w) *w = x;
				x = startX;
				*h += font->newLineHeight;
			}
			if (gStringBuffer[i] != '\n') x += c->w;
		}
	}
	if (x > *w) *w = x;
}

JUFont juFontLoad(const char *filename) {
	JUFont font = juMalloc(sizeof(struct JUFont));

	// nah im not using this

	return font;
}

JUFont juFontLoadFromTexture(VK2DTexture texture, uint32_t unicodeStart, uint32_t unicodeEnd, float w, float h) {
	// Setup font struct
	JUFont font = juMalloc(sizeof(struct JUFont));
	font->characters = juMalloc(sizeof(struct JUCharacter) * (unicodeEnd - unicodeStart));
	font->bitmap = texture;
	font->newLineHeight = h;
	font->unicodeStart = unicodeStart;
	font->unicodeEnd = unicodeEnd;

	// Make sure the texture loaded and the texture has enough space to load the desired characters
	if (font->bitmap != NULL && w * h * (unicodeEnd - unicodeStart) <= vk2dTextureWidth(font->bitmap) * vk2dTextureHeight(font->bitmap)) {
		// Calculate positions of every character in the font
		float x = 0;
		float y = 0;
		int i = unicodeStart;
		while (i < unicodeEnd) {
			font->characters[i - unicodeStart].x = x;
			font->characters[i - unicodeStart].y = y;
			font->characters[i - unicodeStart].w = w;
			font->characters[i - unicodeStart].h = h;
			font->characters[i - unicodeStart].drawn = true;
			font->characters[i - unicodeStart].ykern = 0;
			if (x + w >= vk2dTextureWidth(font->bitmap)) {
				y += h;
				x = 0;
			} else {
				x += w;
			}
			i++;
		}
	} else {
		free(font->characters);
		free(font);
		font = NULL;
	}

	return font;
}

void juFontFree(JUFont font) {
	if (font != NULL) {
		vk2dTextureFree(font->bitmap);
		free(font->characters);
		free(font);
	}
}

static void _juFontDrawInternal(JUFont font, float x, float y, float w, const char *string) {
	// Information needed to draw the text
	float startX = x;
	bool justMadeNewline = false;
	int len = utf8Decode((void*)string, gStringBuffer, gStringBufferSize);

	// Loop through each character and render individually
	for (int i = 0; i < len; i++) {
		if (font->unicodeStart <= gStringBuffer[i] && font->unicodeEnd > gStringBuffer[i]) {
			JUCharacter *c = &font->characters[gStringBuffer[i] - font->unicodeStart];

			// Move to the next line if we're about to go over
			if ((w > 0 && (c->w + x) - startX > w) || gStringBuffer[i] == '\n') {
				x = startX;
				y += font->newLineHeight;
				justMadeNewline = true;
			} else {
				justMadeNewline = false;
			}

			// Draw character (or not) and move the cursor forward
			if (!(gStringBuffer[i] == ' ' && justMadeNewline)) {
				if (c->drawn)
					vk2dRendererDrawTexture(font->bitmap, x, y + c->ykern, 1, 1, 0, 0, 0, c->x, c->y, c->w, c->h);
				if (gStringBuffer[i] != '\n') x += c->w;
			}
		}
	}
}

static void _juFontDrawInternalExt(JUFont font, float x, float y, float w, const char *string) {
	// Information needed to draw the text
	float startX = x;
	bool justMadeNewline = false;
	int len = utf8Decode((void*)string, gStringBuffer, gStringBufferSize);
	int instance = 0;
	float displacementX = 0;
	float displacementY = 0;
	float wave = 0;
	float shake = 0;
	vec4 colour, originalColour;
	vk2dRendererGetColourMod(originalColour);
	bool rainbow = false;

	// Loop through each character and render individually
	for (int i = 0; i < len; i++) {
		if (font->unicodeStart <= gStringBuffer[i] && font->unicodeEnd > gStringBuffer[i]) {
			// String tokens are parsed first
			if (gStringBuffer[i] == '[' && (i == 0 || gStringBuffer[i - 1] != '#')) {
				i += _juFontParseModifierToken(&gStringBuffer[i], colour, &displacementX, &displacementY, &wave, &shake, &rainbow) - 1;
				continue;
			} else if (gStringBuffer[i] == '#' && i < len - 1 && gStringBuffer[i + 1] == '[') {
				continue;
			}

			JUCharacter *c = &font->characters[gStringBuffer[i] - font->unicodeStart];

			// Move to the next line if we're about to go over
			if ((w > 0 && (c->w + x) - startX > w) || gStringBuffer[i] == '\n') {
				x = startX;
				y += font->newLineHeight;
				justMadeNewline = true;
			} else {
				justMadeNewline = false;
			}

			// Draw character (or not) and move the cursor forward
			if (!(gStringBuffer[i] == ' ' && justMadeNewline)) {
				if (c->drawn) {
					float xoff = displacementX;
					float yoff = displacementY;
					if (wave != 0) {
						yoff += sin((juTime() + ((float)instance / 3)) * 3) * wave;
					}
					if (shake != 0) {
						xoff += vk2dRandom(-1, 1) * shake;
						yoff += vk2dRandom(-1, 1) * shake;
					}
					if (rainbow) {
						vec4 tempColour = {gColours[instance % 7][0], gColours[instance % 7][1], gColours[instance % 7][2], gColours[instance % 7][3]};
                        vk2dRendererSetColourMod(tempColour);
					} else {
						vk2dRendererSetColourMod(colour);
					}
                    vk2dRendererDrawTexture(font->bitmap, x + xoff, y + yoff + c->ykern, 1, 1, 0, 0, 0, c->x, c->y, c->w, c->h);
					instance++;
				}
				if (gStringBuffer[i] != '\n') x += c->w;
			}
		}
	}
	vk2dRendererSetColourMod(originalColour);
}

void juFontDraw(JUFont font, float x, float y, const char *fmt, ...) {
	// Var args stuff
	unsigned char buffer[1024];
	va_list va;
	va_start(va, fmt);
	vsprintf((void*)buffer, fmt, va);
	va_end(va);

	_juFontDrawInternal(font, x, y, 0, (void*)buffer);
}

void juFontDrawWrapped(JUFont font, float x, float y, float w, const char *fmt, ...) {
	// Var args stuff
	unsigned char buffer[1024];
	va_list va;
	va_start(va, fmt);
	vsprintf((void*)buffer, fmt, va);
	va_end(va);

	_juFontDrawInternal(font, x, y, w, (void*)buffer);
}

void juFontDrawExt(JUFont font, float x, float y, const char *string) {
	_juFontDrawInternalExt(font, x, y, 0, (void*)string);
}

void juFontDrawWrappedExt(JUFont font, float x, float y, float w, const char *string) {
	_juFontDrawInternalExt(font, x, y, w, (void*)string);
}

/********************** Buffer **********************/

JUBuffer juBufferLoad(const char *filename) {
	JUBuffer buffer = juMalloc(sizeof(struct JUBuffer));
	buffer->data = juGetFile(filename, &buffer->size);

	if (buffer->data == NULL) {
		free(buffer);
		buffer = NULL;
	}

	return buffer;
}

JUBuffer juBufferCreate(void *data, uint32_t size) {
	JUBuffer buffer = juMalloc(sizeof(struct JUBuffer));
	buffer->data = juMalloc(size);
	buffer->size = size;
	memcpy(buffer->data, data, size);
	return buffer;
}

void juBufferSave(JUBuffer buffer, const char *filename) {
	FILE *out = fopen(filename, "wb");

	if (out != NULL) {
		fwrite(buffer->data, buffer->size, 1, out);
		fclose(out);
	} else {
		juLog("Failed to open file \"%s\"", filename);
	}
}

void juBufferFree(JUBuffer buffer) {
	if (buffer != NULL) {
		free(buffer->data);
		free(buffer);
	}
}

void juBufferSaveRaw(void *data, uint32_t size, const char *filename) {
	FILE *out = fopen(filename, "wb");

	if (out != NULL) {
		fwrite(data, size, 1, out);
		fclose(out);
	} else {
		juLog("Failed to open file \"%s\"", filename);
	}
}

/********************** Asset Loader **********************/

// Puts an asset into the loader (properly)
static void juLoaderAdd(JULoader loader, JUAsset asset) {
	uint32_t hash = juHash(asset->name);

	// Either we drop the asset right into its slot or send it down a linked
	// list chain if there is a hash collision
	if (loader->assets[hash] == NULL) {
		loader->assets[hash] = asset;
	} else {
		JUAsset current = loader->assets[hash];
		bool placed = false;
		while (!placed) {
			if (current->next == NULL) {
				placed = true;
				current->next = asset;
			} else {
				current = current->next;
			}
		}
	}
}

// Just gets the raw asset from the loader
static JUAsset juLoaderGet(JULoader loader, const char *key, JUAssetType type) {
	JUAsset current = loader->assets[juHash(key)];
	bool found = false;

	while (!found) {
		if (current != NULL && strcmp(current->name, key) == 0 && current->type == type) // this is the right asset
			found = true;
		else if (current != NULL) // wrong one, next one down the chain
			current = current->next;
		else // it doesn't exist, current should be null at this point
			found = true;
	}

	return current;
}

// Frees a specific asset (not its next one in its chain though)
static void juLoaderAssetFree(JUAsset asset) {
	if (asset->type == JU_ASSET_TYPE_FONT) {
		juFontFree(asset->Asset.font);
	} else if (asset->type == JU_ASSET_TYPE_TEXTURE) {
		vk2dTextureFree(asset->Asset.tex);
	} else if (asset->type == JU_ASSET_TYPE_SOUND) {
		juSoundFree(asset->Asset.sound);
	} else if (asset->type == JU_ASSET_TYPE_BUFFER) {
		juBufferFree(asset->Asset.buffer);
	} else if (asset->type == JU_ASSET_TYPE_SPRITE) {
		juSpriteFree(asset->Asset.sprite);
	}
	free((void*)asset->name);
	free(asset);
}

JULoader juLoaderCreate(JULoadedAsset *files, uint32_t fileCount) {
	JULoader loader = juMalloc(sizeof(struct JULoader));
	JUAsset *assets = juMallocZero(JU_BUCKET_SIZE * sizeof(struct JUAsset));
	loader->assets = assets;

	// Load all assets
	for (int i = 0; i < fileCount; i++) {
		const char *extension = files[i].path + juLastDot(files[i].path) + 1;
		JUAsset asset = juMalloc(sizeof(struct JUAsset));
		asset->name = juCopyString(files[i].path);
		asset->next = NULL;

		// Load file based on asset
		if (strcmp(extension, "jufnt") == 0) {
			asset->type = JU_ASSET_TYPE_FONT;
			asset->Asset.font = juFontLoad(files[i].path);
		} else if (strcmp(extension, "png") == 0 || strcmp(extension, "jpg") == 0 || strcmp(extension, "jpeg") == 0 || strcmp(extension, "bmp") == 0) {
			if (files[i].h + files[i].w + files[i].delay != 0) {
				// Sprite
				asset->type = JU_ASSET_TYPE_SPRITE; // TODO: Check for existing textures with the same name first

				// Check if we already loaded the texture
				JUAsset tex = juLoaderGet(loader, asset->name, JU_ASSET_TYPE_TEXTURE);
				if (tex != NULL)
					asset->Asset.sprite = juSpriteFrom(tex->Asset.tex, files[i].x, files[i].y, files[i].w, files[i].h, files[i].delay, files[i].frames);
				else
					asset->Asset.sprite = juSpriteCreate(files[i].path, files[i].x, files[i].y, files[i].w, files[i].h, files[i].delay, files[i].frames);
				if (asset->Asset.sprite != NULL) {
					asset->Asset.sprite->originX = files[i].originX;
					asset->Asset.sprite->originY = files[i].originY;
				}
			} else {
				// Just a textures
				asset->type = JU_ASSET_TYPE_TEXTURE;
				asset->Asset.tex = vk2dTextureLoad(files[i].path);
			}
		} else if (strcmp(extension, "wav") == 0) {
			asset->type = JU_ASSET_TYPE_SOUND;
			asset->Asset.sound = juSoundLoad(files[i].path);
		} else {
			asset->type = JU_ASSET_TYPE_BUFFER;
			asset->Asset.buffer = juBufferLoad(files[i].path);
		}

		juLoaderAdd(loader, asset);
	}

	return loader;
}

VK2DTexture juLoaderGetTexture(JULoader loader, const char *filename) {
	JUAsset asset = juLoaderGet(loader, filename, JU_ASSET_TYPE_TEXTURE);
	VK2DTexture out = NULL;

	if (asset != NULL) {
		out = asset->Asset.tex;
	} else {
		juLog("Asset \"%s\" was never loaded", filename);
	}

	return out;
}

JUFont juLoaderGetFont(JULoader loader, const char *filename) {
	JUAsset asset = juLoaderGet(loader, filename, JU_ASSET_TYPE_FONT);
	JUFont out = NULL;

	if (asset != NULL) {
		out = asset->Asset.font;
	} else {
		juLog("Asset \"%s\" doesn't exist", filename);
	}

	return out;
}

JUSound juLoaderGetSound(JULoader loader, const char *filename) {
	JUAsset asset = juLoaderGet(loader, filename, JU_ASSET_TYPE_SOUND);
	JUSound out = NULL;

	if (asset != NULL) {
		out = asset->Asset.sound;
	} else {
		juLog("Asset \"%s\" doesn't exist", filename);
	}

	return out;
}

JUBuffer juLoaderGetBuffer(JULoader loader, const char *filename) {
	JUAsset asset = juLoaderGet(loader, filename, JU_ASSET_TYPE_BUFFER);
	JUBuffer out = NULL;

	if (asset != NULL) {
		out = asset->Asset.buffer;
	} else {
		juLog("Asset \"%s\" doesn't exist", filename);
	}

	return out;
}

JUSprite juLoaderGetSprite(JULoader loader, const char *filename) {
	JUAsset asset = juLoaderGet(loader, filename, JU_ASSET_TYPE_SPRITE);
	JUSprite out = NULL;

	if (asset != NULL) {
		out = asset->Asset.sprite;
	} else {
		juLog("Asset \"%s\" doesn't exist", filename);
	}

	return out;
}

void juLoaderFree(JULoader loader) {
	if (loader != NULL) {
		for (int i = 0; i < JU_BUCKET_SIZE; i++) {
			JUAsset current = loader->assets[i];
			while (current != NULL) {
				JUAsset next = current->next;
				juLoaderAssetFree(current);
				current = next;
			}
		}
		free(loader->assets);
	}
}

/********************** Sound **********************/

JUSound juSoundLoad(const char *filename) {
	JUSound sound = juMallocZero(sizeof(struct JUSound));
	sound->sound = cs_load_wav(filename);
	return sound;
}

JUPlayingSound juSoundPlay(JUSound sound, bool loop, float volumeLeft, float volumeRight) {
	sound->soundInfo = cs_make_def(&sound->sound);
	sound->soundInfo.looped = loop;
	sound->soundInfo.volume_left = 0.5 * volumeLeft;
	sound->soundInfo.volume_right = 0.5 * volumeRight;
	JUPlayingSound new = {cs_play_sound(gSoundContext, sound->soundInfo)};
	return new;
}

void juSoundUpdate(JUPlayingSound sound, bool loop, float volumeLeft, float volumeRight) {
	if (sound.playingSound != NULL && cs_is_active(sound.playingSound)) {
		cs_loop_sound(sound.playingSound, loop);
		cs_set_volume(sound.playingSound, volumeLeft, volumeRight);
	}
}

void juSoundStop(JUPlayingSound sound) {
	if (sound.playingSound != NULL && cs_is_active(sound.playingSound))
		cs_stop_sound(sound.playingSound);
}

void juSoundFree(JUSound sound) {
	cs_free_sound(&sound->sound);
	free(sound);
}

void juSoundStopAll() {
	cs_stop_all_sounds(gSoundContext);
}

/********************** Collisions **********************/

double juPointAngle(double x1, double y1, double x2, double y2) {
	return atan2f(x2 - x1, y2 - y1) - (VK2D_PI / 2);
}

double juPointDistance(double x1, double y1, double x2, double y2) {
	return sqrtf(powf(y2 - y1, 2) + powf(x2 - x1, 2));
}

JUPoint2D juRotatePoint(double x, double y, double originX, double originY, double rotation) {
	// Applies a 2D rotation matrix
	JUPoint2D point = {x - originX, y - originY};
	JUPoint2D final;
	final.x = (point.x * cos(-rotation)) - (point.y * sin(-rotation));
	final.y = (point.x * sin(-rotation)) + (point.y * cos(-rotation));
	final.x += originX;
	final.y += originY;
	return final;
}

bool juRectangleCollision(JURectangle *r1, JURectangle *r2) {
	return (r1->y + r1->h > r2->y && r1->y < r2->y + r2->h && r1->x + r1->w > r2->x && r1->x < r2->x + r2->w);
}

bool juRotatedRectangleCollision(JURectangle *r1, double rot1, double originX1, double originY1, JURectangle *r2, double rot2, double originX2, double originY2) {
	// Check if one of the vertices of either rectangle is in the other rectangle
	return false; // TODO: This
}

bool juCircleCollision(JUCircle *c1, JUCircle *c2) {
	return juPointDistance(c1->x, c1->y, c2->x, c2->y) < c1->r + c2->r;
}

bool juPointInRectangle(JURectangle *rect, double x, double y) {
	return (x >= rect->x && x <= rect->x + rect->w && y >= rect->y && y <= rect->y + rect->h);
}

bool juPointInRotatedRectangle(JURectangle *rect, double rot, double originX, double originY, double x, double y) {
	// Here we work in reverse so instead of rotating the rectangle we rotate the point we are checking in reverse about the origin
	double distance = juPointDistance(originX + rect->x, originY + rect->y, x, y);
	double angle = juPointAngle(originX + rect->x, originY + rect->y, x, y);
	double newX = rect->x + juCastX(distance, angle + rot);
	double newY = rect->y + juCastY(distance, angle + rot);
	return juPointInRectangle(rect, newX, newY);
}

bool juPointInCircle(JUCircle *circle, double x, double y) {
	return juPointDistance(circle->x, circle->y, x, y) <= circle->r;
}

double juLerp(double percent, double start, double stop) {
	return start + ((stop - start) * percent);
}

double juSerp(double percent, double start, double stop) {
	return start + ((stop - start) * ((sin((percent * VK2D_PI) - (VK2D_PI / 2)) / 2) + 0.5));
}

double juCastX(double length, double angle) {
	return length * cos(-angle);
}

double juCastY(double length, double angle) {
	return length * sin(-angle);
}

double juSign(double x) {
	return x < 0 ? -1 : (x > 0 ? 1 : 0);
}

double juSubToZero(double x, double y) {
	if (x < 0) {
		return x + y > 0 ? 0 : x + y;
	} else if (x > 0) {
		return x - y < 0 ? 0 : x - y;
	}
	return x;
}

double juClamp(double x, double min, double max) {
	return x < min ? min : (x > max ? max : x);
}

/********************** File I/O **********************/

JUSave juSaveLoad(const char *filename) {
	JUSave save = juMallocZero(sizeof(JUSave));
	FILE *buffer = fopen(filename, "rb");
	char header[6] = {0};

	if (buffer != NULL) {
		// Grab total size
		fread(header, 1, 5, buffer);
		fread(&save->size, 4, 1, buffer);

		// If we don't check for max size its possible for a corrupt file to cause a crash
		if (save->size < JU_SAVE_MAX_SIZE && strcmp("JUSAV", header) == 0) {
			save->data = juMallocZero(sizeof(struct JUData) * save->size);

			// Grab all data
			for (int i = 0; i < save->size && !feof(buffer); i++) {
				JUData *data = &save->data[i];
				int keySize;

				// Get key size and make sure its a legit key
				fread(&keySize, 4, 1, buffer);
				if (keySize <= JU_SAVE_MAX_KEY_SIZE) {
					data->key = juMalloc(keySize + 1);
					((char*)data->key)[keySize] = 0;
					fread((void*)data->key, keySize, 1, buffer);
					fread(&data->type, 4, 1, buffer);

					// Get data depending on what it is
					if (data->type == JU_DATA_TYPE_DOUBLE) {
						fread(&data->Data.f64, 8, 1, buffer);
					} else if (data->type == JU_DATA_TYPE_FLOAT) {
						fread(&data->Data.f32, 4, 1, buffer);
					} else if (data->type == JU_DATA_TYPE_INT64) {
						fread(&data->Data.i64, 8, 1, buffer);
					} else if (data->type == JU_DATA_TYPE_UINT64) {
						fread(&data->Data.u64, 8, 1, buffer);
					} else if (data->type == JU_DATA_TYPE_STRING) {
						int stringLength;
						fread(&stringLength, 4, 1, buffer);
						data->Data.string = juMalloc(stringLength + 1);
						((char*)data->Data.string)[stringLength] = 0;
						fread((void*)data->Data.string, stringLength, 1, buffer);
					} else if (data->type == JU_DATA_TYPE_VOID) {
						fread(&data->Data.data.size, 4, 1, buffer);
						data->Data.string = juMalloc(data->Data.data.size);
						fread((void*)data->Data.data.data, data->Data.data.size, 1, buffer);
					}
				} else {
					juLog("Save file \"%s\" is likely corrupt (key size of %i)", filename, keySize);
				}
			}
		} else {
			juLog("Save file \"%s\" is likely corrupt (save count of %i)", filename, save->size);
			free(save);
			save = NULL;
		}

		fclose(buffer);
	} else {
		juLog("File \"%s\" could not be opened", filename);
	}

	return save;
}

void juSaveStore(JUSave save, const char *filename) {
	FILE *out = fopen(filename, "wb");

	if (out != NULL) {
		// Header data
		fwrite("JUSAV", 5, 1, out);
		fwrite(&save->size, 4, 1, out);

		for (int i = 0; i < save->size; i++) {
			// Write the key for this data
			int size = strlen(save->data[i].key);
			fwrite(&size, 4, 1, out);
			fwrite(save->data[i].key, size, 1, out);
			fwrite(&save->data[i].type, 4, 1, out);

			// Write stuff depending on type of data this is
			if (save->data[i].type == JU_DATA_TYPE_DOUBLE) {
				fwrite(&save->data[i].Data.f64, 8, 1, out);
			} else if (save->data[i].type == JU_DATA_TYPE_FLOAT) {
				fwrite(&save->data[i].Data.f32, 4, 1, out);
			} else if (save->data[i].type == JU_DATA_TYPE_INT64) {
				fwrite(&save->data[i].Data.i64, 8, 1, out);
			} else if (save->data[i].type == JU_DATA_TYPE_UINT64) {
				fwrite(&save->data[i].Data.u64, 8, 1, out);
			} else if (save->data[i].type == JU_DATA_TYPE_STRING) {
				size = strlen(save->data[i].Data.string);
				fwrite(&size, 4, 1, out);
				fwrite(save->data[i].Data.string, size, 1, out);
			} else if (save->data[i].type == JU_DATA_TYPE_VOID) {
				fwrite(&save->data[i].Data.data.size, 4, 1, out);
				fwrite(save->data[i].Data.data.data, save->data[i].Data.data.size, 1, out);
			}
		}

		fclose(out);
	} else {
		juLog("Failed to open file \"%s\"", filename);
	}
}

void juSaveFree(JUSave save) {
	if (save != NULL) {
		for (int i = 0; i < save->size; i++) {
			free((void*)save->data[i].key);
			if (save->data[i].type == JU_DATA_TYPE_STRING)
				free((void*)save->data[i].Data.string);
			else if (save->data[i].type == JU_DATA_TYPE_VOID)
				free(save->data[i].Data.data.data);
		}
		free(save->data);
		free(save);
	}
}

static JUData *juSaveGetRawData(JUSave save, const char *key) {
	JUData *out = NULL;

	for (int i = 0; i < save->size && out == NULL; i++)
		if (strcmp(key, save->data[i].key) == 0)
			out = &save->data[i];

	return out;
}

static void juSaveSetRawData(JUSave save, const char *key, JUData *data) {
	JUData *exists = juSaveGetRawData(save, key);

	if (exists == NULL) {
		save->data = juRealloc(save->data, sizeof(struct JUData) * (save->size + 1));
		memcpy(&save->data[save->size], data, sizeof(struct JUData));
		save->size++;
	} else {
		memcpy(exists, data, sizeof(struct JUData));
	}
}

void juSaveSetInt64(JUSave save, const char *key, int64_t data) {
	JUData out;
	out.key = juCopyString(key);
	out.type = JU_DATA_TYPE_INT64;
	out.Data.i64 = data;
	juSaveSetRawData(save, key, &out);
}

bool juSaveKeyExists(JUSave save, const char *key) {
	return juSaveGetRawData(save, key) != NULL;
}

int64_t juSaveGetInt64(JUSave save, const char *key) {
	JUData *data = juSaveGetRawData(save, key);

	if (data != NULL && data->type == JU_DATA_TYPE_INT64) {
		return data->Data.i64;
	} else if (data != NULL && data->type != JU_DATA_TYPE_INT64) {
		juLog("Requested key \"%s\" does not match expected type INT64", key);
	}

	return 0;
}

void juSaveSetUInt64(JUSave save, const char *key, uint64_t data) {
	JUData out;
	out.key = juCopyString(key);
	out.type = JU_DATA_TYPE_UINT64;
	out.Data.u64 = data;
	juSaveSetRawData(save, key, &out);
}

uint64_t juSaveGetUInt64(JUSave save, const char *key) {
	JUData *data = juSaveGetRawData(save, key);

	if (data != NULL && data->type == JU_DATA_TYPE_UINT64) {
		return data->Data.u64;
	} else if (data != NULL && data->type != JU_DATA_TYPE_UINT64) {
		juLog("Requested key \"%s\" does not match expected type UINT64", key);
	}

	return 0;
}

void juSaveSetFloat(JUSave save, const char *key, float data) {
	JUData out;
	out.key = juCopyString(key);
	out.type = JU_DATA_TYPE_FLOAT;
	out.Data.f32 = data;
	juSaveSetRawData(save, key, &out);
}

float juSaveGetFloat(JUSave save, const char *key) {
	JUData *data = juSaveGetRawData(save, key);

	if (data != NULL && data->type == JU_DATA_TYPE_FLOAT) {
		return data->Data.f32;
	} else if (data != NULL && data->type != JU_DATA_TYPE_FLOAT) {
		juLog("Requested key \"%s\" does not match expected type FLOAT", key);
	}

	return 0;
}

void juSaveSetDouble(JUSave save, const char *key, double data) {
	JUData out;
	out.key = juCopyString(key);
	out.type = JU_DATA_TYPE_DOUBLE;
	out.Data.f64 = data;
	juSaveSetRawData(save, key, &out);
}

double juSaveGetDouble(JUSave save, const char *key) {
	JUData *data = juSaveGetRawData(save, key);

	if (data != NULL && data->type == JU_DATA_TYPE_DOUBLE) {
		return data->Data.f64;
	} else if (data != NULL && data->type != JU_DATA_TYPE_DOUBLE) {
		juLog("Requested key \"%s\" does not match expected type DOUBLE", key);
	}

	return 0;
}

void juSaveSetString(JUSave save, const char *key, const char *data) {
	JUData out;
	out.key = juCopyString(key);
	out.type = JU_DATA_TYPE_STRING;
	out.Data.string = juCopyString(data);
	juSaveSetRawData(save, key, &out);
}

const char *juSaveGetString(JUSave save, const char *key) {
	JUData *data = juSaveGetRawData(save, key);

	if (data != NULL && data->type == JU_DATA_TYPE_STRING) {
		return data->Data.string;
	} else if (data != NULL && data->type != JU_DATA_TYPE_STRING) {
		juLog("Requested key \"%s\" does not match expected type STRING", key);
	}

	return 0;
}

void juSaveSetData(JUSave save, const char *key, void *data, uint32_t size) {
	JUData out;
	out.key = juCopyString(key);
	out.type = JU_DATA_TYPE_VOID;
	out.Data.data.size = size;
	out.Data.data.data = juMalloc(size);
	memcpy(out.Data.data.data, data, out.Data.data.size);
	juSaveSetRawData(save, key, &out);
}

void *juSaveGetData(JUSave save, const char *key, uint32_t *size) {
	JUData *data = juSaveGetRawData(save, key);

	if (data != NULL && data->type == JU_DATA_TYPE_VOID) {
		*size = data->Data.data.size;
		return data->Data.data.data;
	} else if (data != NULL && data->type != JU_DATA_TYPE_VOID) {
		juLog("Requested key \"%s\" does not match expected type VOID", key);
	}

	return 0;
}

/********************** Keyboard **********************/

bool juKeyboardGetKey(SDL_Scancode key) {
	return gKeyboardState[key];
}

bool juKeyboardGetKeyPressed(SDL_Scancode key) {
	return gKeyboardState[key] && !gKeyboardPreviousState[key];
}

bool juKeyboardGetKeyReleased(SDL_Scancode key) {
	return !gKeyboardState[key] && gKeyboardPreviousState[key];
}

/********************** Animations **********************/
JUSprite juSpriteCreate(const char *filename, float x, float y, float w, float h, float delay, int frames) {
	JUSprite spr = juMalloc(sizeof(struct JUSprite));
	spr->Internal.tex = vk2dTextureLoad(filename);

	// Set default values
	if (spr->Internal.tex != NULL) {
		spr->Internal.frames = frames == 0 ? 1 : frames;
		spr->Internal.frame = 0;
		spr->Internal.lastTime = SDL_GetPerformanceCounter();
		spr->x = x;
		spr->y = y;
		spr->Internal.w = w;
		spr->Internal.h = h;
		spr->scaleX = 1;
		spr->scaleY = 1;
		spr->delay = delay;
		spr->rotation = 0;
		spr->Internal.copy = false;
		spr->originX = 0;
		spr->originY = 0;
	} else {
		juLog("Could not create sprite from image \"%s\"", filename);
		free(spr);
		spr = NULL;
	}

	return spr;
}

JUSprite juSpriteFrom(VK2DTexture tex, float x, float y, float w, float h, float delay, int frames) {
	JUSprite spr = juMalloc(sizeof(struct JUSprite));
	spr->Internal.tex = tex;

	// Set default values
	spr->Internal.frames = frames == 0 ? 1 : frames;
	spr->Internal.frame = 0;
	spr->Internal.lastTime = SDL_GetPerformanceCounter();
	spr->x = x;
	spr->y = y;
	spr->Internal.w = w;
	spr->Internal.h = h;
	spr->scaleX = 1;
	spr->scaleY = 1;
	spr->delay = delay;
	spr->rotation = 0;
	spr->originX = 0;
	spr->originY = 0;
	spr->Internal.copy = true;

	return spr;
}

JUSprite juSpriteCopy(JUSprite original) {
	JUSprite spr = juMalloc(sizeof(struct JUSprite));
	memcpy(spr, original, sizeof(struct JUSprite));
	spr->Internal.copy = true;
	return spr;
}

void juSpriteDraw(JUSprite spr, float x, float y) {
	// First we check if we must advance a frame
	if ((double)(SDL_GetPerformanceCounter() - spr->Internal.lastTime) / (double)SDL_GetPerformanceFrequency() >= spr->delay) {
		spr->Internal.frame = spr->Internal.frame == spr->Internal.frames - 1 ? 0 : spr->Internal.frame + 1;
		spr->Internal.lastTime = SDL_GetPerformanceCounter();
	}

	// Calculate where in the texture to draw
	float drawX = roundf(spr->x + ((int)(spr->Internal.frame * spr->Internal.w) % (int)(vk2dTextureWidth(spr->Internal.tex) - spr->x)));
	float drawY = roundf(spr->y + (spr->Internal.h * floorf((spr->Internal.frame * spr->Internal.w) / (vk2dTextureWidth(spr->Internal.tex) - spr->x))));

	vk2dRendererDrawTexture(
			spr->Internal.tex,
			x - (spr->originX * spr->scaleX),
			y - (spr->originY * spr->scaleY),
			spr->scaleX,
			spr->scaleY,
			spr->rotation,
			spr->originX,
			spr->originY,
			drawX,
			drawY,
			spr->Internal.w,
			spr->Internal.h);
}

void juSpriteDrawFrame(JUSprite spr, uint32_t index, float x, float y) {
	if (index >= 0 && index < spr->Internal.frames) {
		// Calculate where in the texture to draw
		float drawX = roundf(spr->x + ((int)(index * spr->Internal.w) % (int)(vk2dTextureWidth(spr->Internal.tex) - spr->x)));
		float drawY = roundf(spr->y + (spr->Internal.h * floorf((index * spr->Internal.w) / (vk2dTextureWidth(spr->Internal.tex) - spr->x))));

		vk2dRendererDrawTexture(
				spr->Internal.tex,
				x - (spr->originX * spr->scaleX),
				y - (spr->originY * spr->scaleY),
				spr->scaleX,
				spr->scaleY,
				spr->rotation,
				spr->originX,
				spr->originY,
				drawX,
				drawY,
				spr->Internal.w,
				spr->Internal.h);
	}
}

void juSpriteFree(JUSprite spr) {
	if (spr != NULL) {
		if (!spr->Internal.copy)
			vk2dTextureFree(spr->Internal.tex);
		free(spr);
	}
}