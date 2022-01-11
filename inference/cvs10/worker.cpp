//
// Created by yuan on 3/11/21.
//

#include "worker.h"
#include "stream_sei.h"

void OneCardInferApp::start(const std::vector<std::string>& urls)
{
    bool enable_outputer = false;
    if (bm::start_with(m_output_url, "rtsp://") || bm::start_with(m_output_url, "udp://") ||
        bm::start_with(m_output_url, "tcp://")) {
        enable_outputer = true;
    }

    m_detectorDelegate->set_detected_callback([this, enable_outputer](bm::FrameInfo &frameInfo) {
        for (int frame_idx = 0; frame_idx < frameInfo.frames.size(); ++frame_idx) {
            int ch = frameInfo.frames[frame_idx].chan_id;

            //std::cout << "chan=" << ch << std::endl;

            m_appStatis.m_chan_statis[ch]++;
            m_appStatis.m_statis_lock.lock();
            m_appStatis.m_total_statis++;
            m_appStatis.m_statis_lock.unlock();
            // tracker
            if (frameInfo.out_datums[frame_idx].obj_rects.size() > 0) {
                m_chans[ch]->tracker->update(frameInfo.out_datums[frame_idx].obj_rects, frameInfo.out_datums[frame_idx].track_rects);
            }
            // display
#if USE_QTGUI
            bm::UIFrame jpgframe;
            jpgframe.jpeg_data = frameInfo.frames[i].jpeg_data;
            jpgframe.chan_id = ch;
            jpgframe.h = frameInfo.frames[i].height;
            jpgframe.w = frameInfo.frames[i].width;
            jpgframe.datum = frameInfo.out_datums[i];
            m_guiReceiver->pushFrame(jpgframe);
#endif

            //

            if (enable_outputer) {

                std::shared_ptr<bm::ByteBuffer> buf = frameInfo.out_datums[frame_idx].toByteBuffer();
                std::string base64_str = bm::base64_enc(buf->data(), buf->size());

                AVPacket sei_pkt;
                av_init_packet(&sei_pkt);
                AVPacket *pkt1 = frameInfo.frames[frame_idx].avpkt;
                av_packet_copy_props(&sei_pkt, pkt1);
                sei_pkt.stream_index = pkt1->stream_index;

                AVCodecID codec_id = m_chans[ch]->decoder->get_video_codec_id();

                if (codec_id == AV_CODEC_ID_H264) {
                    int packet_size = h264sei_calc_packet_size(base64_str.length());
                    AVBufferRef *buf = av_buffer_alloc(packet_size << 1);
                    //assert(packet_size < 16384);
                    int real_size = h264sei_packet_write(buf->data, true, (uint8_t *) base64_str.data(),
                                                         base64_str.length());
                    sei_pkt.data = buf->data;
                    sei_pkt.size = real_size;
                    sei_pkt.buf = buf;

                } else if (codec_id == AV_CODEC_ID_H265) {
                    int packet_size = h264sei_calc_packet_size(base64_str.length());
                    AVBufferRef *buf = av_buffer_alloc(packet_size << 1);
                    int real_size = h265sei_packet_write(buf->data, true, (uint8_t *) base64_str.data(),
                                                         base64_str.length());
                    sei_pkt.data = buf->data;
                    sei_pkt.size = real_size;
                    sei_pkt.buf = buf;
                }

                m_chans[ch]->outputer->InputPacket(&sei_pkt);
                m_chans[ch]->outputer->InputPacket(frameInfo.frames[frame_idx].avpkt);
                av_packet_unref(&sei_pkt);
            }
        }
    });

    // feature register
    m_featureDelegate->set_detected_callback([this](bm::FeatureFrameInfo &frameInfo) {
        for (int i = 0; i < frameInfo.frames.size(); ++i) {
            int ch = frameInfo.frames[i].chan_id;

            m_appStatis.m_chan_feat_stat[ch]++;
            m_appStatis.m_total_feat_stat++;
        }
    });

    //detector
    bm::DetectorParam param;
    int cpu_num = std::thread::hardware_concurrency();
    int tpu_num = 1;
    param.preprocess_thread_num = cpu_num;
    param.preprocess_queue_size = 10*m_channel_num;
    param.inference_thread_num = tpu_num;
    param.inference_queue_size = 4*m_channel_num;
    param.postprocess_thread_num = cpu_num;
    param.postprocess_queue_size = 5*m_channel_num;
    m_inferPipe.init(param, m_detectorDelegate);

    //feature
    m_featurePipe.init(param, m_featureDelegate);

    for(int i = 0; i < m_channel_num; ++i) {
        int ch = m_channel_start + i;
        std::cout << "push id=" << ch << std::endl;
        TChannelPtr pchan = std::make_shared<TChannel>();
        pchan->decoder = new bm::StreamDecoder(ch);
        if (enable_outputer) pchan->outputer = new bm::FfmpegOutputer();
        pchan->channel_id = ch;

        std::string media_file;
        AVDictionary *opts = NULL;
        av_dict_set_int(&opts, "sophon_idx", m_dev_id, 0);
        av_dict_set(&opts, "output_format", "101", 18);
        av_dict_set(&opts, "extra_frame_buffer_num", "5", 0);

        pchan->decoder->set_avformat_opend_callback([this, pchan](AVFormatContext *ifmt) {
            if (pchan->outputer) {
                size_t pos = m_output_url.rfind(":");
                std::string base_url = m_output_url.substr(0, pos);
                int base_port = std::strtol(m_output_url.substr(pos + 1).c_str(), 0, 10);
                std::string url = bm::format("%s:%d", base_url.c_str(), base_port + pchan->channel_id);
                pchan->outputer->OpenOutputStream(url, ifmt);
            }
        });

        pchan->decoder->set_avformat_closed_callback([this, pchan]() {
            if (pchan->outputer) pchan->outputer->CloseOutputStream();
        });


        pchan->decoder->set_decoded_frame_callback([this, pchan, ch](const AVPacket* pkt, const AVFrame *frame){
            bm::FrameBaseInfo fbi;
            fbi.seq = pchan->seq++;
            fbi.chan_id = ch;
            if (m_skipN > 0) {
                if (fbi.seq % (m_skipN+1) != 0) fbi.skip = true;
            }

#if 0
            if (ch == 0) std::cout << " seq = " << fbi.seq << " skip= " << fbi.skip << std::endl;
#endif
            if(!fbi.skip) {
                fbi.avframe = av_frame_alloc();
                fbi.avpkt = av_packet_alloc();
                av_frame_ref(fbi.avframe, frame);
                av_packet_ref(fbi.avpkt, pkt);
                m_inferPipe.push_frame(&fbi);
            }

            uint64_t current_time = bm::gettime_msec();
            if (current_time - m_chans[ch]->m_last_feature_time > m_feature_delay) {
                for(int feat_idx = 0; feat_idx < m_feature_num; feat_idx++) {
                    bm::FeatureFrame featureFrame;
                    featureFrame.chan_id = ch;
                    featureFrame.seq++;
                    featureFrame.img = cv::imread("face.jpeg", cv::IMREAD_COLOR, m_dev_id);
                    if (featureFrame.img.empty()) {
                        printf("ERROR:Can't find face.jpg in workdir!\n");
                        exit(0);
                    }
                    m_featurePipe.push_frame(&featureFrame);
                }

                m_chans[ch]->m_last_feature_time = current_time;
            }


        });

        pchan->decoder->open_stream(urls[i % urls.size()], true, opts);
        av_dict_free(&opts);
        //pchan->decoder->open_stream("rtsp://admin:hk123456@11.73.11.99/test", false, opts);
        //std::this_thread::sleep_for(std::chrono::seconds(100));
        m_chans[ch] = pchan;
    }
}