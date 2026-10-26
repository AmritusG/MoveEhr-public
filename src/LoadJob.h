#pragma once
// Background model loading. One worker thread per distinct file; plugin instances asking for the
// same file (same path + mtime) share the job and its result.
#include "ModelData.h"
#include <atomic>
#include <memory>
#include <string>

namespace mehr
{
struct LoadTicket
{
	std::string path;
	std::string key; // path + mtime (+ texture limit), identifies the file version
	int maxTexture = 0;
	bool movements = false; // MoveEhr W8 B: Movement Source + Same Folder
	std::atomic< bool > done{ false };
	std::atomic< bool > cancel{ false };
	std::shared_ptr< const ModelData > model; // valid once done (nullptr on failure)
	std::string error;
	std::string status; // W21: generated models: the one-line report for the Status param
	double msTotal = 0;
};

// Cache key for a file on disk loaded with a texture limit ("" if the file does not exist).
// MoveEhr W8 B: + "|moves" with Movement Source + Same Folder (the folder's files are not in the key: Reload picks up a new one)
std::string ModelKey( const std::string& path, int maxTexture, bool movements = false );

// Starts (or joins) a load of `path`. `forceFresh` skips any job already running for it (Reload).
std::shared_ptr< LoadTicket > RequestLoad( const std::string& path, int maxTexture, bool forceFresh, bool movements = false );

// Called at bundle unload: cancels running jobs and waits for workers to exit.
void ShutdownLoaders();
}
