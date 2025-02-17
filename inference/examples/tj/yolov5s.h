//
// Created by yuan on 2/22/21.
//

#include "bmutility.h"
#include "inference2.h"
#include "opencv2/opencv.hpp"
#include <sys/time.h>
#include "face_common.h"
#include "hiredis/hiredis.h"

/*
struct FrameBaseInfo {
    int chan_id;
    uint64_t seq;
    AVPacket *avpkt;
    AVFrame *avframe;
    bm::DataPtr jpeg_data;
    int height, width;
    FrameBaseInfo() {
        chan_id = 0;
        seq = 0;
        avpkt = 0;
        width = height = 0;
    }
};

struct FrameInfo {
    std::vector<FrameBaseInfo> frames;
    std::vector<bm_tensor_t> input_tensors;
    std::vector<bm_tensor_t> output_tensors;
    std::vector<bm::NetOutputDatum> datums;
};
 */

class YoloV5 : public bm::DetectorDelegate<bm::FrameInfo2> {
    int MAX_BATCH = 1;
    bm::BMNNContextPtr m_bmctx;
    bm::BMNNNetworkPtr m_bmnet;
    int m_net_h, m_net_w;
    bool m_isLastDetector = false;

    // configuration
    float m_confThreshold = 0.5;
    float m_nmsThreshold = 0.5;
    float m_objThreshold = 0.5;
    std::vector<std::string> m_class_names;
    std::vector<std::string> person_class_names{
        "person", "smoke", "fire"};
    std::vector<std::string> phone_class_names{
        "phone", "smoking", "extinguisher"};
    std::vector<std::string> yqhz10701_class_names{
        "person", "nohelmet", "fire", "smoke", "helmet"};
    std::vector<std::string> yqhz10702_class_names{
        "phone", "smoking", "sleep", "upclothes", "lowerclothes"};

    int m_class_num = 3; // default is coco names
    // const float m_anchors[3][6] = {{10.0, 13.0, 16.0, 30.0, 33.0, 23.0},
    // {30.0, 61.0, 62.0, 45.0, 59.0, 119.0},{116.0, 90.0, 156.0, 198.0, 373.0,
    // 326.0}};
    std::vector<std::vector<std::vector<int>>> m_anchors{
        {{10, 13}, {16, 30}, {33, 23}},
        {{30, 61}, {62, 45}, {59, 119}},
        {{116, 90}, {156, 198}, {373, 326}}};
    const int m_anchor_num = 3;

public:
    YoloV5(bm::BMNNContextPtr bmctx, int max_batch = 1);
    ~YoloV5();

    virtual int preprocess(std::vector<bm::FrameInfo2> &frames) override;
    virtual int forward(std::vector<bm::FrameInfo2> &frame_info) override;
    virtual int postprocess(std::vector<bm::FrameInfo2> &frame_info) override;

    void setLastDetector(bool isLast);
    void setConfidence(float confidence, float obj_threshold, float nms_threshold) {
        m_confThreshold = confidence;
        m_nmsThreshold = nms_threshold;
        m_objThreshold = obj_threshold;
    }

    std::string detectorName;
    std::string channelId;
    std::string redisTopic;

private:
    float sigmoid(float x);
    int argmax(float *data, int dsize);
    static float get_aspect_scaled_ratio(int src_w, int src_h, int dst_w,
                                         int dst_h, bool *alignWidth);
    void NMS(bm::NetOutputObjects &dets, float nmsConfidence);
    void free_fwds(std::vector<bm::NetForward> &ios);

    void extract_yolobox_cpu(bm::FrameInfo2 &frameInfo);

    std::string get_label(int class_id) {
        std::string label_name = "";
        if (detectorName == "person") {
            label_name = person_class_names[class_id];
        } else if (detectorName == "phone") {
            label_name = phone_class_names[class_id];
        } else if (detectorName == "yqhz10701") {
            label_name = yqhz10701_class_names[class_id];
        } else if (detectorName == "yqhz10702") {
            label_name = yqhz10702_class_names[class_id];
        }
        return label_name;
    }
};
