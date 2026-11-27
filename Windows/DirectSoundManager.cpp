#include "Common.h"
#include "DirectSoundManager.h"
#include "Core/Shared/Audio/SoundMixer.h"
#include "Core/Shared/Emulator.h"
#include "Core/Shared/EmuSettings.h"
#include "Core/Shared/MessageManager.h"

DirectSoundManager::DirectSoundManager(Emulator* emu, HWND hwnd)
{
	_emu = emu;
	_hWnd = hwnd;
	_directSound = 0;
	_primaryBuffer = 0;
	_secondaryBuffer = 0;

	memset(&_audioDeviceID, 0, sizeof(_audioDeviceID));

	if(InitializeDirectSound(48000, true)) {
		_emu->GetSoundMixer()->RegisterAudioDevice(this);
	} else {
		MessageManager::DisplayMessage("Error", "CouldNotInitializeAudioSystem");
	}
}

DirectSoundManager::~DirectSoundManager()
{
	if(_emu && _emu->GetSoundMixer()) {
		_emu->GetSoundMixer()->RegisterAudioDevice(nullptr);
	}
	Release();
}

bool CALLBACK DirectSoundManager::DirectSoundEnumProc(LPGUID lpGUID, LPCWSTR lpszDesc, LPCSTR lpszDrvName, LPVOID lpContext)
{
	vector<SoundDeviceInfo>* devices = (vector<SoundDeviceInfo>*)lpContext;

	SoundDeviceInfo deviceInfo;
	deviceInfo.description = utf8::utf8::encode(lpszDesc);
	if(lpGUID != nullptr) {
		memcpy((void*)&deviceInfo.guid, lpGUID, 16);
	} else {
		memset((void*)&deviceInfo.guid, 0, 16);
	}
	devices->push_back(deviceInfo);

	return true;
}

vector<DirectSoundManager::SoundDeviceInfo> DirectSoundManager::GetAvailableDeviceInfo()
{
	vector<SoundDeviceInfo> devices;
	DirectSoundEnumerateW((LPDSENUMCALLBACKW)DirectSoundManager::DirectSoundEnumProc, &devices);
	return devices;
}

string DirectSoundManager::GetAvailableDevices()
{
	string deviceString;
	for(SoundDeviceInfo device : GetAvailableDeviceInfo()) {
		deviceString += device.description + "||"s;
	}
	return deviceString;
}

void DirectSoundManager::SetAudioDevice(string deviceName)
{
	if(_audioDeviceName == deviceName) {
		return;
	}

	bool found = false;
	_audioDeviceName = deviceName;
	for(SoundDeviceInfo device : GetAvailableDeviceInfo()) {
		if(device.description == deviceName) {
			found = true;
			if(memcmp(&_audioDeviceID, &device.guid, 16) != 0) {
				memcpy(&_audioDeviceID, &device.guid, 16);
				_needReset = true;
			}
			break;
		}
	}

	if(!found) {
		memset(&_audioDeviceID, 0, sizeof(_audioDeviceID));
		_needReset = true;
	}
}

bool DirectSoundManager::InitializeDirectSound(uint32_t sampleRate, bool isStereo)
{
	HRESULT result;
	DSBUFFERDESC bufferDesc;
	WAVEFORMATEX waveFormat;

	// Initialize the direct sound interface pointer for the default sound device.
	result = DirectSoundCreate8(&_audioDeviceID, &_directSound, NULL);
	if(FAILED(result)) {
		MessageManager::Log("[Audio] Failed to create direct sound device.");
		return false;
	}

	// Set the cooperative level to priority so the format of the primary sound buffer can be modified.
	result = _directSound->SetCooperativeLevel(_hWnd, DSSCL_PRIORITY);
	if(FAILED(result)) {
		MessageManager::Log("[Audio] Failed to set cooperative level.");
		return false;
	}

	// Setup the primary buffer description.
	bufferDesc.dwSize = sizeof(DSBUFFERDESC);
	bufferDesc.dwFlags = DSBCAPS_PRIMARYBUFFER | DSBCAPS_CTRLVOLUME;
	bufferDesc.dwBufferBytes = 0;
	bufferDesc.dwReserved = 0;
	bufferDesc.lpwfxFormat = NULL;
	bufferDesc.guid3DAlgorithm = GUID_NULL;

	// Get control of the primary sound buffer on the default sound device.
	result = _directSound->CreateSoundBuffer(&bufferDesc, &_primaryBuffer, NULL);
	if(FAILED(result)) {
		MessageManager::Log("[Audio] Failed to create primary sound buffer.");
		return false;
	}

	// Setup the format of the primary sound bufffer.
	_sampleRate = sampleRate;
	_isStereo = isStereo;

	waveFormat.wFormatTag = WAVE_FORMAT_PCM;
	waveFormat.nSamplesPerSec = _sampleRate;
	waveFormat.wBitsPerSample = 16;
	waveFormat.nChannels = isStereo ? 2 : 1;
	waveFormat.nBlockAlign = (waveFormat.wBitsPerSample / 8) * waveFormat.nChannels;
	waveFormat.nAvgBytesPerSec = waveFormat.nSamplesPerSec * waveFormat.nBlockAlign;
	waveFormat.cbSize = 0;

	// Set the primary buffer to be the wave format specified.
	result = _primaryBuffer->SetFormat(&waveFormat);
	if(FAILED(result)) {
		MessageManager::Log("[Audio] Failed to set the sound format.");
		return false;
	}

	int32_t latency = _emu->GetSettings()->GetAudioConfig().AudioLatency;
	int32_t requestedByteLatency = (int32_t)((float)(sampleRate * latency) / 1000.0f * waveFormat.nBlockAlign);
	_bufferSize = (int32_t)std::ceil((double)requestedByteLatency * 2 / 0x10000) * 0x10000;

	// Set the buffer description of the secondary sound buffer that the wave file will be loaded onto.
	bufferDesc.dwSize = sizeof(DSBUFFERDESC);
	bufferDesc.dwFlags = DSBCAPS_CTRLPOSITIONNOTIFY | DSBCAPS_GETCURRENTPOSITION2 | DSBCAPS_GLOBALFOCUS | DSBCAPS_LOCSOFTWARE | DSBCAPS_CTRLVOLUME | DSBCAPS_CTRLFREQUENCY;
	bufferDesc.dwBufferBytes = _bufferSize;
	bufferDesc.dwReserved = 0;
	bufferDesc.lpwfxFormat = &waveFormat;
	bufferDesc.guid3DAlgorithm = GUID_NULL;

	// Create a temporary sound buffer with the specific buffer settings.
	IDirectSoundBuffer* tempBuffer;
	result = _directSound->CreateSoundBuffer(&bufferDesc, &tempBuffer, NULL);
	if(FAILED(result)) {
		MessageManager::Log("[Audio] Failed to create temporary sound buffer.");
		return false;
	}

	// Test the buffer format against the direct sound 8 interface and create the secondary buffer.
	result = tempBuffer->QueryInterface(IID_IDirectSoundBuffer8, (LPVOID*)&_secondaryBuffer);
	if(FAILED(result)) {
		MessageManager::Log("[Audio] Failed to obtain secondary sound buffer.");
		return false;
	}

	// Set volume of the buffer to 100%.
	result = _secondaryBuffer->SetVolume(DSBVOLUME_MAX);
	if(FAILED(result)) {
		MessageManager::Log("[Audio] Failed to set volume of the secondary sound buffer.");
		return false;
	}

	// Release the temporary buffer.
	tempBuffer->Release();

	_playing = false;

	return ClearSecondaryBuffer();
}

void DirectSoundManager::Release()
{
	_playing = false;
	_needReset = false;
	_lastWriteOffset = 0;
	_lastPlayOffset = 0;
	_minimumLatency = 0;
	ResetStats();

	if(_secondaryBuffer) {
		_secondaryBuffer->Release();
		_secondaryBuffer = nullptr;
	}

	if(_primaryBuffer) {
		_primaryBuffer->Release();
		_primaryBuffer = nullptr;
	}

	if(_directSound) {
		_directSound->Release();
		_directSound = nullptr;
	}
}

bool DirectSoundManager::ClearSecondaryBuffer()
{
	unsigned char* bufferPtr;
	DWORD bufferSize;
	if(FAILED(_secondaryBuffer->Lock(0, 0, (void**)&bufferPtr, &bufferSize, nullptr, nullptr, DSBLOCK_ENTIREBUFFER))) {
		return false;
	}
	memset(bufferPtr, 0, bufferSize);
	if(FAILED(_secondaryBuffer->Unlock(bufferPtr, bufferSize, nullptr, 0)) || FAILED(_secondaryBuffer->SetCurrentPosition(0))) {
		return false;
	}
	_lastWriteOffset = 0;
	_lastPlayOffset = 0;
	return true;
}

void DirectSoundManager::CopyToSecondaryBuffer(const uint8_t* data, uint32_t size)
{
	uint8_t* bufferPtrA;
	uint8_t* bufferPtrB;
	DWORD bufferASize;
	DWORD bufferBSize;

	if(FAILED(_secondaryBuffer->Lock(_lastWriteOffset, size, (void**)&bufferPtrA, &bufferASize, (void**)&bufferPtrB, &bufferBSize, 0))) {
		_needReset = true;
		return;
	}

	memcpy(bufferPtrA, data, bufferASize);
	if(bufferPtrB && bufferBSize > 0) {
		memcpy(bufferPtrB, data + bufferASize, bufferBSize);
	}

	if(FAILED(_secondaryBuffer->Unlock(bufferPtrA, bufferASize, bufferPtrB, bufferBSize))) {
		_needReset = true;
		return;
	}
	_lastWriteOffset = (_lastWriteOffset + size) % _bufferSize;
}

void DirectSoundManager::Pause()
{
	if(_secondaryBuffer) {
		_secondaryBuffer->Stop();
	}
	_playing = false;
}

void DirectSoundManager::Stop()
{
	if(!_playing && _lastWriteOffset == 0 && _averageLatency == 0) {
		return;
	}

	if(_secondaryBuffer) {
		if(FAILED(_secondaryBuffer->Stop()) || !ClearSecondaryBuffer()) {
			_needReset = true;
		}
	}

	_playing = false;
	ResetStats();
}

void DirectSoundManager::Play()
{
	if(_secondaryBuffer && SUCCEEDED(_secondaryBuffer->Play(0, 0, DSBPLAY_LOOPING))) {
		_playing = true;
	}
}

uint32_t DirectSoundManager::GetBufferDistance(DWORD from, DWORD to) const
{
	return (to + _bufferSize - from) % _bufferSize;
}

uint32_t DirectSoundManager::GetMinimumLatency() const
{
	//Use the settings screen's recommended minimum until the device reports a write lead.
	return _minimumLatency > 0 ? _minimumLatency : 55;
}

AudioStatistics DirectSoundManager::GetStatistics()
{
	AudioStatistics stats = BaseSoundManager::GetStatistics();
	stats.MinimumLatency = GetMinimumLatency();
	return stats;
}

void DirectSoundManager::RecoverFromUnderrun(DWORD playCursor, uint32_t safeLead)
{
	uint32_t queuedBytes = GetBufferDistance(_lastPlayOffset, _lastWriteOffset);
	uint32_t playAdvance = GetBufferDistance(_lastPlayOffset, playCursor);
	_lastPlayOffset = playCursor;
	if(_playing && playAdvance + safeLead > queuedBytes) {
		//The driver has passed the queued audio. Clear old samples before restarting.
		uint32_t underruns = _bufferUnderrunEventCount + 1;
		Stop();
		_bufferUnderrunEventCount = underruns;
	}
}

void DirectSoundManager::ProcessEndOfFrame()
{
	DWORD playCursor;
	if(_playing && SUCCEEDED(_secondaryBuffer->GetCurrentPosition(&playCursor, nullptr))) {
		ProcessLatency(playCursor, _lastWriteOffset);
	}

	AudioConfig& cfg = _emu->GetSettings()->GetAudioConfig();
	SetAudioDevice(cfg.AudioDevice);

	uint32_t targetLatency = std::max(cfg.AudioLatency, GetMinimumLatency());
	if(_averageLatency > 0 && !_emu->GetSettings()->IsFastForward() && std::abs(_averageLatency - targetLatency) > 50) {
		//Latency is way off (over 50ms gap), stop audio & start again
		Stop();
	}
}

void DirectSoundManager::PlayBuffer(int16_t* soundBuffer, uint32_t sampleCount, uint32_t sampleRate, bool isStereo)
{
	uint32_t bytesPerFrame = sizeof(int16_t) * (isStereo ? 2 : 1);
	uint32_t latency = _emu->GetSettings()->GetAudioConfig().AudioLatency;
	if(!_secondaryBuffer || _sampleRate != sampleRate || _isStereo != isStereo || _needReset || latency != _previousLatency) {
		_previousLatency = latency;
		Release();
		if(!InitializeDirectSound(sampleRate, isStereo)) {
			Release();
			return;
		}
	}

	DWORD playCursor, safeWriteCursor;
	if(FAILED(_secondaryBuffer->GetCurrentPosition(&playCursor, &safeWriteCursor))) {
		_needReset = true;
		return;
	}
	uint32_t safeLead = GetBufferDistance(playCursor, safeWriteCursor);
	if(_playing && safeLead > 0 && sampleCount > 0) {
		//Reserve two audio blocks beyond the device write cursor for scheduling jitter.
		uint32_t minimumBytes = std::min(_bufferSize / 2, safeLead + 2 * sampleCount * bytesPerFrame);
		uint32_t bytesPerSecond = sampleRate * bytesPerFrame;
		_minimumLatency = (minimumBytes * 1000 + bytesPerSecond - 1) / bytesPerSecond;
	}
	RecoverFromUnderrun(playCursor, safeLead);
	if(_needReset) {
		return;
	}

	uint32_t queuedBytes = GetBufferDistance(_lastPlayOffset, _lastWriteOffset);
	uint32_t freeFrames = (_bufferSize - queuedBytes) / bytesPerFrame;
	//Leave one frame free so coincident cursors always mean an empty buffer.
	if(sampleCount > 0 && freeFrames > 1) {
		uint32_t writeFrames = std::min(sampleCount, freeFrames - 1);
		CopyToSecondaryBuffer((const uint8_t*)soundBuffer, writeFrames * bytesPerFrame);
	}

	if(!_playing && !_needReset) {
		uint32_t targetLatency = std::max(latency, GetMinimumLatency());
		uint32_t targetBytes = (sampleRate * targetLatency / 1000) * bytesPerFrame;
		if(GetBufferDistance(_lastPlayOffset, _lastWriteOffset) >= targetBytes) {
			Play();
		}
	}
}
