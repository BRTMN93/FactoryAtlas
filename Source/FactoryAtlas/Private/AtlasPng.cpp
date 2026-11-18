// A streaming PNG encoder over zlib, for map exports too big to encode in one piece.

#include "AtlasPng.h"

#include "HAL/FileManager.h"
#include "HAL/PlatformFileManager.h"
#include "GenericPlatform/GenericPlatformFile.h"
#include "Misc/Paths.h"

THIRD_PARTY_INCLUDES_START
#include "zlib.h"
THIRD_PARTY_INCLUDES_END

namespace Atlas
{
	struct FPngStreamState
	{
		z_stream Z = {};
		bool bZlib = false;
		bool bOk = false;
		TUniquePtr<IFileHandle> File;
		FString Path;
		int32 Width = 0;
		int32 Height = 0;
		int32 Rows = 0;
		TArray<uint8> Previous; // The last row as RGBA, for the Paeth filter
		TArray<uint8> Current;
		TArray<uint8> Filtered; // Filter byte, then the filtered row
		TArray<uint8> Out;      // Deflate output, written as one IDAT chunk each time it fills

		~FPngStreamState()
		{
			if (bZlib)
			{
				deflateEnd(&Z);
			}
		}

		bool Write(const void* Data, int64 Bytes)
		{
			bOk = bOk && File && File->Write(static_cast<const uint8*>(Data), Bytes);
			return bOk;
		}

		bool WriteBigEndian(uint32 Value)
		{
			const uint8 Bytes[4] = { uint8(Value >> 24), uint8(Value >> 16), uint8(Value >> 8), uint8(Value) };
			return Write(Bytes, 4);
		}

		bool WriteChunk(const char* Type, const uint8* Data, uint32 Bytes)
		{
			uLong Crc = crc32(0, reinterpret_cast<const Bytef*>(Type), 4);
			if (Bytes > 0)
			{
				Crc = crc32(Crc, Data, Bytes);
			}
			return WriteBigEndian(Bytes) && Write(Type, 4) && (Bytes == 0 || Write(Data, Bytes)) && WriteBigEndian(uint32(Crc));
		}

		/** Deflates what is in Z's input. Finish flushes the stream's end. */
		bool Deflate(bool bFinish)
		{
			for (;;)
			{
				Z.next_out = Out.GetData();
				Z.avail_out = uInt(Out.Num());
				const int Result = deflate(&Z, bFinish ? Z_FINISH : Z_NO_FLUSH);
				if (Result == Z_STREAM_ERROR)
				{
					bOk = false;
					return false;
				}
				const uint32 Have = uint32(Out.Num()) - Z.avail_out;
				if (Have > 0 && !WriteChunk("IDAT", Out.GetData(), Have))
				{
					return false;
				}
				if (bFinish ? Result == Z_STREAM_END : (Z.avail_out != 0 && Z.avail_in == 0))
				{
					return true;
				}
			}
		}
	};

	FPngStreamWriter::FPngStreamWriter() = default;

	FPngStreamWriter::~FPngStreamWriter()
	{
		if (State && State->File)
		{
			// Never closed: whatever was written is not a whole image.
			State->File.Reset();
			IFileManager::Get().Delete(*State->Path);
		}
	}

	bool FPngStreamWriter::Open(const FString& Path, int32 Width, int32 Height, int32 Level)
	{
		State = MakeUnique<FPngStreamState>();
		FPngStreamState& S = *State;
		S.Path = Path;
		S.Width = Width;
		S.Height = Height;
		if (Width <= 0 || Height <= 0)
		{
			return false;
		}

		IPlatformFile& Platform = FPlatformFileManager::Get().GetPlatformFile();
		Platform.CreateDirectoryTree(*FPaths::GetPath(Path));
		S.File.Reset(Platform.OpenWrite(*Path));
		if (!S.File)
		{
			return false;
		}
		if (deflateInit(&S.Z, FMath::Clamp(Level, 1, 9)) != Z_OK)
		{
			return false;
		}
		S.bZlib = true;
		S.bOk = true;

		const int32 RowBytes = Width * 4;
		S.Previous.SetNumZeroed(RowBytes);
		S.Current.SetNumUninitialized(RowBytes);
		S.Filtered.SetNumUninitialized(RowBytes + 1);
		S.Out.SetNumUninitialized(1 << 20);

		static const uint8 Signature[8] = { 0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n' };
		uint8 Header[13];
		const auto Put = [&Header](int32 At, uint32 Value)
		{
			Header[At] = uint8(Value >> 24);
			Header[At + 1] = uint8(Value >> 16);
			Header[At + 2] = uint8(Value >> 8);
			Header[At + 3] = uint8(Value);
		};
		Put(0, uint32(Width));
		Put(4, uint32(Height));
		Header[8] = 8;   // Bits per channel
		Header[9] = 6;   // RGBA
		Header[10] = 0;  // Deflate
		Header[11] = 0;  // Adaptive filtering, one filter byte per row
		Header[12] = 0;  // Not interlaced
		return S.Write(Signature, 8) && S.WriteChunk("IHDR", Header, 13);
	}

	bool FPngStreamWriter::WriteRows(const FColor* Pixels, int32 NumRows)
	{
		if (!State || !State->bOk)
		{
			return false;
		}
		FPngStreamState& S = *State;
		NumRows = FMath::Min(NumRows, S.Height - S.Rows);
		const int32 RowBytes = S.Width * 4;
		for (int32 Row = 0; Row < NumRows; Row++)
		{
			const FColor* Source = Pixels + int64(Row) * S.Width;
			uint8* Raw = S.Current.GetData();
			for (int32 X = 0; X < S.Width; X++)
			{
				Raw[X * 4 + 0] = Source[X].R;
				Raw[X * 4 + 1] = Source[X].G;
				Raw[X * 4 + 2] = Source[X].B;
				Raw[X * 4 + 3] = Source[X].A;
			}

			// Paeth on every row: it does well on both the flat shapes and the rendered imagery.
			const uint8* Up = S.Previous.GetData();
			uint8* Dest = S.Filtered.GetData();
			Dest[0] = 4;
			for (int32 i = 0; i < RowBytes; i++)
			{
				const int32 A = i >= 4 ? Raw[i - 4] : 0;
				const int32 B = Up[i];
				const int32 C = i >= 4 ? Up[i - 4] : 0;
				const int32 P = A + B - C;
				const int32 PA = FMath::Abs(P - A), PB = FMath::Abs(P - B), PC = FMath::Abs(P - C);
				const int32 Predicted = (PA <= PB && PA <= PC) ? A : (PB <= PC ? B : C);
				Dest[i + 1] = uint8(Raw[i] - Predicted);
			}
			Swap(S.Previous, S.Current);

			S.Z.next_in = S.Filtered.GetData();
			S.Z.avail_in = uInt(S.Filtered.Num());
			if (!S.Deflate(false))
			{
				return false;
			}
			S.Rows++;
		}
		return S.bOk;
	}

	bool FPngStreamWriter::Close()
	{
		if (!State)
		{
			return false;
		}
		FPngStreamState& S = *State;
		bool bOk = S.bOk && S.Rows == S.Height;
		if (bOk)
		{
			S.Z.next_in = nullptr;
			S.Z.avail_in = 0;
			bOk = S.Deflate(true) && S.WriteChunk("IEND", nullptr, 0);
		}
		if (S.File)
		{
			bOk = S.File->Flush() && bOk;
			S.File.Reset();
		}
		if (!bOk)
		{
			IFileManager::Get().Delete(*S.Path);
		}
		State.Reset();
		return bOk;
	}
}
