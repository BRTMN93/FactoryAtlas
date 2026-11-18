#pragma once

#include "CoreMinimal.h"

namespace Atlas
{
	struct FPngStreamState;

	/**
	 * Writes an 8-bit RGBA PNG a band of rows at a time, so an export never holds the whole image. A 32768 px
	 * image is 4 GB of pixels, more than the engine's own PNG encoder can take in one piece (it copies it, too).
	 * Not thread safe: one writer is used from one task at a time.
	 */
	class FPngStreamWriter
	{
	public:
		FPngStreamWriter();
		~FPngStreamWriter();

		bool Open(const FString& Path, int32 Width, int32 Height, int32 Level = 4);

		/** NumRows rows of Width pixels, top to bottom, in the engine's BGRA order. False once anything failed. */
		bool WriteRows(const FColor* Pixels, int32 NumRows);

		/** Finishes the file. False if anything failed or the image is short of rows; the file is deleted then. */
		bool Close();

	private:
		TUniquePtr<FPngStreamState> State;
	};
}
