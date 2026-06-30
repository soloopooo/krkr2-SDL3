// Stubs for KRMovie subsystem - implements all virtual functions as no-ops
#include "../../movie/ffmpeg/KRMovieLayer.h"

NS_KRMOVIE_BEGIN

TVPMoviePlayer::TVPMoviePlayer() {}
TVPMoviePlayer::~TVPMoviePlayer() {}
void TVPMoviePlayer::Release() {}
void TVPMoviePlayer::SetPosition(uint64_t) {}
void TVPMoviePlayer::GetPosition(uint64_t*) {}
void TVPMoviePlayer::GetStatus(tTVPVideoStatus*) {}
void TVPMoviePlayer::Rewind() {}
void TVPMoviePlayer::SetFrame(int) {}
void TVPMoviePlayer::GetFrame(int*) {}
void TVPMoviePlayer::GetFPS(double*) {}
void TVPMoviePlayer::GetNumberOfFrame(int*) {}
void TVPMoviePlayer::GetTotalTime(int64_t*) {}
void TVPMoviePlayer::GetVideoSize(long*, long*) {}
void TVPMoviePlayer::SetPlayRate(double) {}
void TVPMoviePlayer::GetPlayRate(double*) {}
void TVPMoviePlayer::SetAudioBalance(long) {}
void TVPMoviePlayer::GetAudioBalance(long*) {}
void TVPMoviePlayer::SetAudioVolume(long) {}
void TVPMoviePlayer::GetAudioVolume(long*) {}
void TVPMoviePlayer::GetNumberOfAudioStream(uint64_t*) {}
void TVPMoviePlayer::SelectAudioStream(uint64_t) {}
void TVPMoviePlayer::GetNumberOfVideoStream(uint64_t*) {}
void TVPMoviePlayer::SelectVideoStream(uint64_t) {}
void TVPMoviePlayer::GetEnableAudioStreamNum(long*) {}
void TVPMoviePlayer::GetEnableVideoStreamNum(long*) {}
void TVPMoviePlayer::DisableAudioStream() {}
void TVPMoviePlayer::SetLoopSegement(int, int) {}
void TVPMoviePlayer::Flush() {}
void TVPMoviePlayer::FrameMove() {}
int TVPMoviePlayer::WaitForBuffer(volatile std::atomic_bool&, int) { return 0; }
void TVPMoviePlayer::BitmapPicture::swap(BitmapPicture&) {}
void TVPMoviePlayer::BitmapPicture::Clear() {}
int TVPMoviePlayer::AddVideoPicture(DVDVideoPicture&, int) { return 0; }

VideoPresentLayer::~VideoPresentLayer() {}
tTVPBaseTexture* VideoPresentLayer::GetFrontBuffer() { return nullptr; }
void VideoPresentLayer::SetVideoBuffer(tTVPBaseTexture*, tTVPBaseTexture*, long) {}
void VideoPresentLayer::OnContinuousCallback(tjs_uint64) {}
int VideoPresentLayer::AddVideoPicture(DVDVideoPicture&, int) { return 0; }

void MoviePlayerLayer::BuildGraph(tTJSNI_VideoOverlay*, IStream*, const tjs_char*, const tjs_char*, uint64_t) {}
void MoviePlayerLayer::OnPlayEvent(KRMovieEvent, void*) {}
void MoviePlayerLayer::Play() {}

VideoPresentOverlay::~VideoPresentOverlay() {}
void VideoPresentOverlay::Play() {}
void VideoPresentOverlay::Stop() {}
VideoPresentOverlay2* VideoPresentOverlay2::create() { return nullptr; }
void VideoPresentOverlay2::SetRootNode(cocos2d::Node*) {}

NS_KRMOVIE_END
