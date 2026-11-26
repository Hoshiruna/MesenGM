#pragma once

#include "Common.h"
#include "Core/Shared/Audio/BaseSoundManager.h"

class Emulator;

class DirectSoundManager final : public BaseSoundManager
{
private:
	struct SoundDeviceInfo
	{
		string description;
		GUID guid;
	};

public:
	DirectSoundManager(Emulator* emu, HWND hWnd);
	~DirectSoundManager();

	void Release();
	void ProcessEndOfFrame();
	void PlayBuffer(int16_t* soundBuffer, uint32_t bufferSize, uint32_t sampleRate, bool isStereo);
	void Play();
	void Pause();
	void Stop();

	string GetAvailableDevices();
	void SetAudioDevice(string deviceName);
	AudioStatistics GetStatistics() override;

private:
	vector<SoundDeviceInfo> GetAvailableDeviceInfo();
	static bool CALLBACK DirectSoundEnumProc(LPGUID lpGUID, LPCWSTR lpszDesc, LPCSTR lpszDrvName, LPVOID lpContext);
	bool InitializeDirectSound(uint32_t sampleRate, bool isStereo);
	bool ClearSecondaryBuffer();
	void CopyToSecondaryBuffer(const uint8_t* data, uint32_t size);
	uint32_t GetBufferDistance(DWORD from, DWORD to) const;
	uint32_t GetMinimumLatency() const;
	void RecoverFromUnderrun(DWORD playCursor, uint32_t safeLead);

private:
	Emulator* _emu;
	HWND _hWnd;
	GUID _audioDeviceID;
	bool _needReset = false;
	string _audioDeviceName = "";

	DWORD _lastWriteOffset = 0;
	DWORD _lastPlayOffset = 0;
	uint32_t _previousLatency = 0;
	uint32_t _minimumLatency = 0;
	bool _playing = false;

	IDirectSound8* _directSound;
	IDirectSoundBuffer* _primaryBuffer;
	IDirectSoundBuffer8* _secondaryBuffer;
};
