/// \file WrenPreprocessor.c
/// \author Paolo Mazzon
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "src/WrenPreprocessor.h"

const char WREN_SOURCE_HEADER[] = "import \"lib/Audio\" for AudioData, Audio\n"
								  "import \"lib/Drawing\" for Surface, Font, BitmapFont, Sprite, Texture\n"
								  "import \"lib/Engine\" for Engine, Level, Entity\n"
								  "import \"lib/Util\" for Hitbox, Math, Tileset, Buffer\n"
								  "import \"lib/File\" for File, INI\n"
								  "import \"lib/Input\" for Mouse, Keyboard, Gamepad\n"
								  "import \"lib/Renderer\" for Renderer, Camera, Shader\n"
		  						  "import \"Assets\" for Assets\n\n";

const char WREN_SOURCE_FOOTER[] = "";

const char* loadFile(const char *filename) {
	FILE* file = fopen(filename, "rb");
	unsigned char *buffer = NULL;
	int size = 0;

	if (file != NULL) {
		// Find file size
		fseek(file, 0, SEEK_END);
		size = ftell(file);
		rewind(file);

		buffer = malloc(size + 1);
		buffer[size] = 0;

		if (buffer != NULL) {
			// Fill the buffer
			fread(buffer, 1, size, file);
		}
		fclose(file);
	}

	return (const char*)buffer;
}

const char *vksk_PreprocessSource(const char *filename) {
	if (strcmp(filename, "prog/lib/Renderer.wren") != 0 && strcmp(filename, "prog/lib/Engine.wren") != 0 && strcmp(filename, "prog/lib/Util.wren") != 0 &&
			strcmp(filename, "prog/lib/Input.wren") != 0 && strcmp(filename, "prog/lib/File.wren") != 0 && strcmp(filename, "prog/lib/Drawing.wren") != 0 &&
			strcmp(filename, "prog/lib/Audio.wren") != 0 && strcmp(filename, "prog/lib/Tiled.wren") != 0 && strcmp(filename, "Assets.wren") != 0) {
		const char *temp = loadFile(filename);
		char *final = malloc(strlen(temp) + strlen(WREN_SOURCE_FOOTER) + strlen(WREN_SOURCE_HEADER) + 1);
		strncpy(final, WREN_SOURCE_HEADER, strlen(WREN_SOURCE_HEADER));
		strncpy(final + strlen(WREN_SOURCE_HEADER), temp, strlen(temp));
		strncpy(final + strlen(WREN_SOURCE_HEADER) + +strlen(temp), WREN_SOURCE_FOOTER, strlen(WREN_SOURCE_FOOTER));
		final[strlen(temp) + strlen(WREN_SOURCE_FOOTER) + strlen(WREN_SOURCE_HEADER)] = 0;
		free((void *) temp);
		return final;
	} else {
		return loadFile(filename);
	}
}