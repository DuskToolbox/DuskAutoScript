#include "PaddleOcrImpl.h"
#include "IDasOcrResultImpl.h"
#include "IDasOcrResultVectorImpl.h"
#include "IDasReadOnlyStringVectorImpl.h"
#include "IDasSessionImpl.h"
#include "IDasTensorImpl.h"
#include "IDasTensorVectorImpl.h"

#include <das/Core/Logger/Logger.h>
#include <das/DasPtr.hpp>
#include <das/DasString.hpp>
#include <das/Utils/CommonUtils.hpp>

DAS_DISABLE_WARNING_BEGIN

DAS_IGNORE_OPENCV_WARNING
#include <opencv2/core/mat.hpp>
#include <opencv2/imgproc.hpp>

DAS_DISABLE_WARNING_END

#include <algorithm>
#include <cmath>
#include <fstream>
#include <numeric>

DAS_CORE_ORTWRAPPER_NS_BEGIN

// ===== DB post-processing constants (aligned with PaddleOCR db_postprocess.py)
// =====
static constexpr float kDbThreshold = 0.3f;
static constexpr float kBoxThresh = 0.5f;
static constexpr float kUnclipRatio = 2.0f;
static constexpr int   kMaxContours = 10000;
static constexpr int   kMaxImageDim = 16384;

// ===== Detection normalization: ImageNet (Pitfall 3) =====
static constexpr double kDetMean[] = {0.485, 0.456, 0.406};
static constexpr double kDetStd[] = {0.229, 0.224, 0.225};

// ===== Recognition normalization: (pixel/255 - 0.5) / 0.5 (Pitfall 3) =====
static constexpr double kRecMean[] = {127.5, 127.5, 127.5};
static constexpr double kRecStd[] = {127.5, 127.5, 127.5};

// ===== Helper: query tensor dimensions =====
static void GetTensorDims(
    Das::ExportInterface::IDasTensor* tensor,
    std::vector<int64_t>&             dims)
{
    uint32_t rank = 0;
    tensor->GetRank(&rank);
    dims.resize(rank);
    for (uint32_t i = 0; i < rank; ++i)
    {
        tensor->GetDim(i, &dims[i]);
    }
}

// ===== Helper: BoxScoreFast — mean probability inside candidate box =====
// Aligned with PaddleOCR db_postprocess.py: box_score_fast()
static float BoxScoreFast(
    const cv::Mat&                  bitmap,
    const std::vector<cv::Point2f>& box)
{
    float x_coords[4], y_coords[4];
    for (int i = 0; i < 4; ++i)
    {
        x_coords[i] = box[i].x;
        y_coords[i] = box[i].y;
    }

    int xmin = std::max(
        0,
        static_cast<int>(
            std::floor(*std::min_element(x_coords, x_coords + 4))));
    int xmax = std::min(
        bitmap.cols - 1,
        static_cast<int>(std::ceil(*std::max_element(x_coords, x_coords + 4))));
    int ymin = std::max(
        0,
        static_cast<int>(
            std::floor(*std::min_element(y_coords, y_coords + 4))));
    int ymax = std::min(
        bitmap.rows - 1,
        static_cast<int>(std::ceil(*std::max_element(y_coords, y_coords + 4))));

    if (xmax <= xmin || ymax <= ymin)
    {
        return 0.0f;
    }

    cv::Mat mask = cv::Mat::zeros(ymax - ymin + 1, xmax - xmin + 1, CV_8UC1);
    std::vector<cv::Point> shifted_box(4);
    for (int i = 0; i < 4; ++i)
    {
        shifted_box[i] = cv::Point(
            static_cast<int>(box[i].x - xmin),
            static_cast<int>(box[i].y - ymin));
    }
    std::vector<std::vector<cv::Point>> contours = {shifted_box};
    cv::fillPoly(mask, contours, cv::Scalar(1));

    cv::Scalar mean_val = cv::mean(
        bitmap(cv::Rect(xmin, ymin, xmax - xmin + 1, ymax - ymin + 1)),
        mask);
    return static_cast<float>(mean_val[0]);
}

// ===== Helper: DB unclip — expand polygon by distance =====
// distance = area * unclip_ratio / perimeter (aligned with PaddleOCR)
static std::vector<cv::Point2f> UnclipBox(
    const std::vector<cv::Point2f>& box,
    float                           unclip_ratio)
{
    double area = cv::contourArea(box);
    double perimeter = cv::arcLength(box, true);
    if (perimeter < 1e-6)
    {
        return box;
    }

    double distance = area * unclip_ratio / perimeter;

    // Offset each edge outward by distance
    std::vector<cv::Point2f> expanded;
    int                      n = static_cast<int>(box.size());
    for (int i = 0; i < n; ++i)
    {
        cv::Point2f prev = box[(i - 1 + n) % n];
        cv::Point2f curr = box[i];
        cv::Point2f next = box[(i + 1) % n];

        // Direction vectors
        cv::Point2f d1 = (curr - prev);
        cv::Point2f d2 = (next - curr);

        // Normalize
        double len1 = std::sqrt(d1.x * d1.x + d1.y * d1.y);
        double len2 = std::sqrt(d2.x * d2.x + d2.y * d2.y);
        if (len1 < 1e-6 || len2 < 1e-6)
        {
            expanded.push_back(curr);
            continue;
        }
        d1 /= static_cast<float>(len1);
        d2 /= static_cast<float>(len2);

        // Normal (outward) — perpendicular to edge direction
        cv::Point2f n1(-d1.y, d1.x);
        cv::Point2f n2(-d2.y, d2.x);

        // Average normal at corner
        cv::Point2f avg_normal = (n1 + n2) * 0.5f;
        double      avg_len = std::sqrt(
            avg_normal.x * avg_normal.x + avg_normal.y * avg_normal.y);
        if (avg_len < 1e-6)
        {
            expanded.push_back(curr);
            continue;
        }
        avg_normal /= static_cast<float>(avg_len);

        expanded.push_back(curr + avg_normal * static_cast<float>(distance));
    }
    return expanded;
}

// ===== Helper: sort boxes by reading order (top-to-bottom, left-to-right)
// ===== Aligned with PaddleOCR sorted_boxes()
struct DetBox
{
    cv::Rect rect;
    std::vector<cv::Point2f>
        corners; // 4 corner points in original image coords
};

static void SortBoxesByReadingOrder(std::vector<DetBox>& boxes)
{
    std::sort(
        boxes.begin(),
        boxes.end(),
        [](const DetBox& a, const DetBox& b)
        {
            float a_cy = static_cast<float>(a.rect.y + a.rect.height / 2.0);
            float b_cy = static_cast<float>(b.rect.y + b.rect.height / 2.0);
            float avg_h =
                static_cast<float>((a.rect.height + b.rect.height) / 2.0);
            // Same row if y-difference < half average height
            if (std::abs(a_cy - b_cy) < avg_h * 0.5f)
            {
                return a.rect.x < b.rect.x; // left-to-right within row
            }
            return a_cy < b_cy; // top-to-bottom
        });
}

// ===== Helper: CTC greedy decode =====
// Aligned with PaddleOCR rec_postprocess.py: CTC decode
// argmax -> skip blank(0) -> collapse repeats -> dict mapping
struct CtcResult
{
    std::string         text;
    double              score;
    std::vector<double> char_scores; // per-decoded-char score
};

static CtcResult CtcGreedyDecode(
    const float*                    logits,
    int                             time_steps,
    int                             num_classes,
    const std::vector<std::string>& dict)
{
    CtcResult result;
    result.score = 0.0;
    int   last_idx = -1;
    float score_sum = 0.0f;
    int   valid_count = 0;

    for (int t = 0; t < time_steps; ++t)
    {
        const float* step = logits + t * num_classes;

        // Argmax
        int   argmax_idx = 0;
        float max_val = step[0];
        for (int c = 1; c < num_classes; ++c)
        {
            if (step[c] > max_val)
            {
                max_val = step[c];
                argmax_idx = c;
            }
        }

        // CTC: skip blank (index 0), collapse consecutive repeats
        if (argmax_idx == 0)
        {
            // Pitfall 4: index 0 = blank label
            last_idx = argmax_idx;
            continue;
        }
        if (last_idx >= 0 && argmax_idx == last_idx)
        {
            // Collapse consecutive repeat
            continue;
        }

        // Dict mapping (T-64-13: guard against out-of-range)
        if (argmax_idx < static_cast<int>(dict.size()))
        {
            result.text += dict[argmax_idx];
            result.char_scores.push_back(static_cast<double>(max_val));
            score_sum += max_val;
            ++valid_count;
        }

        last_idx = argmax_idx;
    }

    result.score =
        valid_count > 0 ? static_cast<double>(score_sum) / valid_count : 0.0;
    return result;
}

// ===== Helper: create a session Run call wrapper =====
static DasResult RunSession(
    Das::ExportInterface::IDasSession*       session,
    const std::vector<std::string>&          input_name_strs,
    Das::ExportInterface::IDasTensor*        input_tensor,
    const std::vector<std::string>&          output_name_strs,
    Das::ExportInterface::IDasTensorVector** pp_outputs)
{
    // Build input names vector
    auto input_names = new IDasReadOnlyStringVectorImpl(input_name_strs);
    input_names->AddRef();
    Das::DasPtr<Das::ExportInterface::IDasReadOnlyStringVector> input_names_ptr(
        input_names);

    // Build inputs vector
    auto inputs = new IDasTensorVectorImpl();
    inputs->AddRef();
    inputs->AddTensor(input_tensor);
    Das::DasPtr<Das::ExportInterface::IDasTensorVector> inputs_ptr(inputs);

    // Build output names vector
    auto output_names = new IDasReadOnlyStringVectorImpl(output_name_strs);
    output_names->AddRef();
    Das::DasPtr<Das::ExportInterface::IDasReadOnlyStringVector>
        output_names_ptr(output_names);

    auto result = session->Run(input_names, inputs, output_names, pp_outputs);
    return result;
}

// ===== Helper: run recognition on a single text crop =====
static DasResult RecognizeTextCrop(
    Das::ExportInterface::IDasAI* /*ai*/,
    Das::ExportInterface::IDasSession* rec_session,
    const std::vector<std::string>&    rec_output_names,
    int64_t                            rec_input_height,
    int64_t                            rec_input_width,
    const cv::Mat&                     crop,
    const std::vector<std::string>&    dict,
    CtcResult&                         out_result)
{
    // Resize to rec_input_height, preserving aspect ratio
    double scale = static_cast<double>(rec_input_height) / crop.rows;
    int    target_w = std::min(
        static_cast<int>(crop.cols * scale),
        static_cast<int>(rec_input_width));
    target_w = std::max(target_w, 1);

    cv::Mat resized;
    cv::resize(
        crop,
        resized,
        cv::Size(target_w, static_cast<int>(rec_input_height)));

    // Create IDasImage from the resized cv::Mat — we use CreateTensorFromImage
    // which takes IDasImage*. We wrap the cv::Mat data as an
    // IDasImage-compatible buffer. Instead, we can directly compute the float
    // tensor ourselves. For simplicity and to use ai_->CreateTensorFromImage,
    // we create a temporary shape and call it. But CreateTensorFromImage
    // expects IDasImage*, so we need to handle this differently.

    // Actually, CreateTensorFromImage takes raw pixel data from IDasImage.
    // Since we have a cv::Mat (resized), we need to pass the pixel data.
    // We already have the raw pixel bytes. We can create a float tensor
    // directly.

    // Build shape [1, 3, H, W]
    int64_t shape[] = {
        1,
        3,
        static_cast<int64_t>(resized.rows),
        static_cast<int64_t>(resized.cols)};
    int64_t total_elements = shape[0] * shape[1] * shape[2] * shape[3];

    // Preprocess: (pixel / 255.0 - mean) / std  (Pitfall 3: rec normalization)
    std::vector<float> float_data(static_cast<size_t>(total_elements));
    int                height = resized.rows;
    int                width = resized.cols;
    int                channels = resized.channels();

    for (int h = 0; h < height; ++h)
    {
        for (int w = 0; w < width; ++w)
        {
            for (int c = 0; c < 3 && c < channels; ++c)
            {
                auto src_byte =
                    static_cast<double>(resized.at<cv::Vec3b>(h, w)[c]);
                float val = static_cast<float>(
                    (src_byte / 255.0 - kRecMean[c] / 255.0)
                    / (kRecStd[c] / 255.0));
                auto dst_idx =
                    static_cast<size_t>(c * height * width + h * width + w);
                if (dst_idx < float_data.size())
                {
                    float_data[dst_idx] = val;
                }
            }
        }
    }

    // Create ORT tensor directly (bypass CreateTensorFromImage since we have
    // cv::Mat)
    auto input_tensor = Ort::Value::CreateTensor<float>(
        Ort::MemoryInfo::CreateCpu(
            OrtAllocatorType::OrtArenaAllocator,
            OrtMemType::OrtMemTypeCPU),
        float_data.data(),
        static_cast<size_t>(total_elements),
        shape,
        4);

    auto* tensor_impl = new IDasTensorImpl(std::move(input_tensor));
    tensor_impl->AddRef();
    Das::DasPtr<Das::ExportInterface::IDasTensor> tensor_ptr(tensor_impl);

    // Run rec inference
    // Discover rec model input name — use first input name from session
    // PaddleOCR rec model input name is typically "x"
    std::vector<std::string> rec_input_names = {"x"};

    Das::DasPtr<Das::ExportInterface::IDasTensorVector> outputs;
    auto                                                result = RunSession(
        rec_session,
        rec_input_names,
        tensor_impl,
        rec_output_names,
        outputs.Put());
    if (Das::IsFailed(result))
    {
        DAS_CORE_LOG_ERROR("Rec inference failed: result={}", result);
        return result;
    }

    // Get output tensor
    Das::DasPtr<Das::ExportInterface::IDasTensor> out_tensor;
    outputs->GetAt(0, out_tensor.Put());

    // Get output shape and data
    std::vector<int64_t> out_dims;
    GetTensorDims(out_tensor.Get(), out_dims);

    void*    out_data = nullptr;
    uint64_t out_size = 0;
    out_tensor->GetRawData(&out_data, &out_size);

    auto* logits = static_cast<float*>(out_data);

    // Output shape: [1, T, num_classes] or [T, num_classes]
    int time_steps = 1;
    int num_classes = 1;
    if (out_dims.size() == 3)
    {
        time_steps = static_cast<int>(out_dims[1]);
        num_classes = static_cast<int>(out_dims[2]);
    }
    else if (out_dims.size() == 2)
    {
        time_steps = static_cast<int>(out_dims[0]);
        num_classes = static_cast<int>(out_dims[1]);
    }

    // CTC greedy decode (Pitfall 4: blank = index 0)
    out_result = CtcGreedyDecode(logits, time_steps, num_classes, dict);
    return DAS_S_OK;
}

// =====================================================================
// PaddleOcrImpl constructor
// =====================================================================
PaddleOcrImpl::PaddleOcrImpl(
    Das::ExportInterface::IDasAI*      ai,
    Das::ExportInterface::IDasSession* det_session,
    Das::ExportInterface::IDasSession* rec_session,
    std::vector<std::string>           dict)
    : ai_(Das::DasPtr<Das::ExportInterface::IDasAI>(ai)), dict_(std::move(dict))
{
    if (det_session)
    {
        det_session_ =
            Das::DasPtr<Das::ExportInterface::IDasSession>(det_session);
    }
    if (rec_session)
    {
        rec_session_ =
            Das::DasPtr<Das::ExportInterface::IDasSession>(rec_session);
    }
}

// =====================================================================
// PaddleOcrImpl::Recognize — full det+rec pipeline
// =====================================================================
DasResult PaddleOcrImpl::Recognize(
    Das::ExportInterface::IDasImage*            p_image,
    Das::ExportInterface::IDasOcrResultVector** pp_results)
{
    DAS_UTILS_CHECK_POINTER(p_image);
    DAS_UTILS_CHECK_POINTER(pp_results);

    try
    {
        // Get image dimensions
        Das::ExportInterface::DasSize img_size{};
        auto                          cr = p_image->GetSize(&img_size);
        if (Das::IsFailed(cr))
        {
            DAS_CORE_LOG_ERROR("Recognize: GetSize failed: result={}", cr);
            return cr;
        }

        // Image dimension safety check (T-64-12)
        if (img_size.width > kMaxImageDim || img_size.height > kMaxImageDim
            || img_size.width <= 0 || img_size.height <= 0)
        {
            DAS_CORE_LOG_ERROR(
                "Recognize: invalid image size: {}x{}",
                img_size.width,
                img_size.height);
            return DAS_E_INVALID_ARGUMENT;
        }

        // Get raw pixel data
        Das::DasPtr<Das::ExportInterface::IDasBinaryBuffer> buf;
        cr = p_image->GetBinaryBuffer(buf.Put());
        if (Das::IsFailed(cr))
        {
            DAS_CORE_LOG_ERROR(
                "Recognize: GetBinaryBuffer failed: result={}",
                cr);
            return cr;
        }

        unsigned char* raw_data = nullptr;
        buf->GetData(&raw_data);
        uint64_t data_size = 0;
        buf->GetSize(&data_size);

        int img_w = img_size.width;
        int img_h = img_size.height;

        // Construct cv::Mat from raw image data
        cv::Mat image(img_h, img_w, CV_8UC3, raw_data);

        std::vector<DetBox> detected_boxes;

        // ====== Detection phase (skip if only_rec mode) ======
        if (det_session_.Get() != nullptr)
        {
            // --- Det preprocess: resize with aspect ratio, height aligned to
            // 32 ---
            float scale_h = static_cast<float>(det_input_height_)
                            / static_cast<float>(img_h);
            float scale_w = static_cast<float>(det_input_width_)
                            / static_cast<float>(img_w);
            float scale = std::min(scale_h, scale_w);

            int target_h = static_cast<int>(img_h * scale);
            int target_w = static_cast<int>(img_w * scale);

            // Align height to 32
            target_h = (target_h / 32) * 32;
            target_w = (target_w / 32) * 32;
            target_h = std::max(target_h, 32);
            target_w = std::max(target_w, 32);

            cv::Mat resized;
            cv::resize(image, resized, cv::Size(target_w, target_h));

            // Create input tensor via ai_->CreateTensorFromImage
            // Build an IDasImage wrapper for the resized cv::Mat
            // Since CreateTensorFromImage takes IDasImage*, we instead
            // build the float tensor directly and wrap it.
            int64_t shape[] = {
                1,
                3,
                static_cast<int64_t>(target_h),
                static_cast<int64_t>(target_w)};
            int64_t total_elements = shape[0] * shape[1] * shape[2] * shape[3];

            std::vector<float> float_data(static_cast<size_t>(total_elements));

            // Det normalization: ImageNet (Pitfall 3)
            int channels = resized.channels();
            for (int h = 0; h < target_h; ++h)
            {
                for (int w = 0; w < target_w; ++w)
                {
                    for (int c = 0; c < 3 && c < channels; ++c)
                    {
                        auto src_byte =
                            static_cast<double>(resized.at<cv::Vec3b>(h, w)[c]);
                        float val = static_cast<float>(
                            (src_byte / 255.0 - kDetMean[c]) / kDetStd[c]);
                        auto dst_idx = static_cast<size_t>(
                            c * target_h * target_w + h * target_w + w);
                        if (dst_idx < float_data.size())
                        {
                            float_data[dst_idx] = val;
                        }
                    }
                }
            }

            auto det_input_tensor = Ort::Value::CreateTensor<float>(
                Ort::MemoryInfo::CreateCpu(
                    OrtAllocatorType::OrtArenaAllocator,
                    OrtMemType::OrtMemTypeCPU),
                float_data.data(),
                static_cast<size_t>(total_elements),
                shape,
                4);

            auto* det_tensor_impl =
                new IDasTensorImpl(std::move(det_input_tensor));
            det_tensor_impl->AddRef();
            Das::DasPtr<Das::ExportInterface::IDasTensor> det_tensor_ptr(
                det_tensor_impl);

            // --- Det inference ---
            std::vector<std::string> det_input_names = {"x"};

            Das::DasPtr<Das::ExportInterface::IDasTensorVector> det_outputs;
            cr = RunSession(
                det_session_.Get(),
                det_input_names,
                det_tensor_impl,
                det_output_names_,
                det_outputs.Put());
            if (Das::IsFailed(cr))
            {
                DAS_CORE_LOG_ERROR("Det inference failed: result={}", cr);
                return cr;
            }

            // --- DB post-processing ---
            Das::DasPtr<Das::ExportInterface::IDasTensor> det_out_tensor;
            det_outputs->GetAt(0, det_out_tensor.Put());

            std::vector<int64_t> det_out_dims;
            GetTensorDims(det_out_tensor.Get(), det_out_dims);

            void*    det_out_data = nullptr;
            uint64_t det_out_size = 0;
            det_out_tensor->GetRawData(&det_out_data, &det_out_size);

            auto* prob_map = static_cast<float*>(det_out_data);

            // prob_map shape: [1, 1, prob_h, prob_w]
            int prob_h = 1, prob_w = 1;
            if (det_out_dims.size() >= 2)
            {
                prob_h =
                    static_cast<int>(det_out_dims[det_out_dims.size() - 2]);
                prob_w =
                    static_cast<int>(det_out_dims[det_out_dims.size() - 1]);
            }

            // Binarize probability map
            cv::Mat bitmap(prob_h, prob_w, CV_8UC1);
            for (int i = 0; i < prob_h * prob_w; ++i)
            {
                bitmap.data[i] = prob_map[i] > kDbThreshold ? 255 : 0;
            }

            // Find contours
            std::vector<std::vector<cv::Point>> contours;
            cv::findContours(
                bitmap,
                contours,
                cv::RETR_LIST,
                cv::CHAIN_APPROX_SIMPLE);

            // Limit contour count (T-64-14)
            if (contours.size() > static_cast<size_t>(kMaxContours))
            {
                DAS_CORE_LOG_WARN(
                    "Too many contours: {}, truncating to {}",
                    contours.size(),
                    kMaxContours);
                contours.resize(kMaxContours);
            }

            // Scale factors from prob map to original image
            float ratio_h =
                static_cast<float>(img_h) / static_cast<float>(prob_h);
            float ratio_w =
                static_cast<float>(img_w) / static_cast<float>(prob_w);

            // Process each contour
            for (auto& contour : contours)
            {
                if (contour.size() < 4)
                {
                    continue;
                }

                // Minimum area rect
                cv::RotatedRect rotated_rect = cv::minAreaRect(contour);

                // Get 4 corners
                cv::Point2f vertices[4];
                rotated_rect.points(vertices);
                std::vector<cv::Point2f> box(vertices, vertices + 4);

                // Box score (BoxScoreFast algorithm)
                float box_score = BoxScoreFast(
                    bitmap,
                    std::vector<cv::Point2f>{
                        cv::Point2f(
                            rotated_rect.center.x - rotated_rect.size.width / 2,
                            rotated_rect.center.y
                                - rotated_rect.size.height / 2),
                        cv::Point2f(
                            rotated_rect.center.x + rotated_rect.size.width / 2,
                            rotated_rect.center.y
                                - rotated_rect.size.height / 2),
                        cv::Point2f(
                            rotated_rect.center.x + rotated_rect.size.width / 2,
                            rotated_rect.center.y
                                + rotated_rect.size.height / 2),
                        cv::Point2f(
                            rotated_rect.center.x - rotated_rect.size.width / 2,
                            rotated_rect.center.y
                                + rotated_rect.size.height / 2),
                    });

                if (box_score < kBoxThresh)
                {
                    continue;
                }

                // DB unclip
                auto expanded = UnclipBox(box, kUnclipRatio);
                if (expanded.size() < 4)
                {
                    continue;
                }

                // Get bounding rect of expanded box in original image coords
                cv::Rect br = cv::boundingRect(expanded);

                // Scale to original image coordinates
                int x = std::max(0, static_cast<int>(br.x * ratio_w));
                int y = std::max(0, static_cast<int>(br.y * ratio_h));
                int w =
                    std::min(img_w - x, static_cast<int>(br.width * ratio_w));
                int h =
                    std::min(img_h - y, static_cast<int>(br.height * ratio_h));

                if (w <= 0 || h <= 0)
                {
                    continue;
                }

                // Scale corners to original image coordinates
                std::vector<cv::Point2f> scaled_corners;
                for (auto& pt : expanded)
                {
                    scaled_corners.emplace_back(pt.x * ratio_w, pt.y * ratio_h);
                }

                DetBox db;
                db.rect = cv::Rect(x, y, w, h);
                db.corners = scaled_corners;
                detected_boxes.push_back(db);
            }

            // Sort by reading order
            SortBoxesByReadingOrder(detected_boxes);
        }
        else
        {
            // ====== only_rec mode (OCR-04/D-14): use entire image ======
            DetBox db;
            db.rect = cv::Rect(0, 0, img_w, img_h);
            db.corners = {
                cv::Point2f(0, 0),
                cv::Point2f(static_cast<float>(img_w), 0),
                cv::Point2f(
                    static_cast<float>(img_w),
                    static_cast<float>(img_h)),
                cv::Point2f(0, static_cast<float>(img_h))};
            detected_boxes.push_back(db);
        }

        // ====== Recognition phase ======
        auto* result_vector = new IDasOcrResultVectorImpl{};
        result_vector->AddRef();
        result_vector->Reserve(detected_boxes.size());

        for (auto& det_box : detected_boxes)
        {
            // Crop text region from image
            cv::Rect safe_rect =
                det_box.rect & cv::Rect(0, 0, image.cols, image.rows);
            if (safe_rect.width <= 0 || safe_rect.height <= 0)
            {
                continue;
            }

            cv::Mat crop;

            if (det_box.corners.size() == 4 && det_session_.Get() != nullptr)
            {
                // Perspective transform for accurate crop
                // Estimate aspect ratio from corners
                float top_width = std::sqrt(
                    (det_box.corners[1].x - det_box.corners[0].x)
                        * (det_box.corners[1].x - det_box.corners[0].x)
                    + (det_box.corners[1].y - det_box.corners[0].y)
                          * (det_box.corners[1].y - det_box.corners[0].y));
                float bot_width = std::sqrt(
                    (det_box.corners[2].x - det_box.corners[3].x)
                        * (det_box.corners[2].x - det_box.corners[3].x)
                    + (det_box.corners[2].y - det_box.corners[3].y)
                          * (det_box.corners[2].y - det_box.corners[3].y));
                float max_w = std::max(top_width, bot_width);

                float left_height = std::sqrt(
                    (det_box.corners[3].x - det_box.corners[0].x)
                        * (det_box.corners[3].x - det_box.corners[0].x)
                    + (det_box.corners[3].y - det_box.corners[0].y)
                          * (det_box.corners[3].y - det_box.corners[0].y));
                float right_height = std::sqrt(
                    (det_box.corners[2].x - det_box.corners[1].x)
                        * (det_box.corners[2].x - det_box.corners[1].x)
                    + (det_box.corners[2].y - det_box.corners[1].y)
                          * (det_box.corners[2].y - det_box.corners[1].y));
                float max_h = std::max(left_height, right_height);

                // Target crop size
                cv::Point2f dst_pts[4] = {
                    cv::Point2f(0, 0),
                    cv::Point2f(max_w, 0),
                    cv::Point2f(max_w, max_h),
                    cv::Point2f(0, max_h)};
                cv::Point2f src_pts[4] = {
                    det_box.corners[0],
                    det_box.corners[1],
                    det_box.corners[2],
                    det_box.corners[3]};

                cv::Mat transform =
                    cv::getPerspectiveTransform(src_pts, dst_pts);
                cv::warpPerspective(
                    image,
                    crop,
                    transform,
                    cv::Size(static_cast<int>(max_w), static_cast<int>(max_h)));
            }
            else
            {
                // Simple rectangular crop (only_rec mode)
                crop = image(safe_rect).clone();
            }

            if (crop.empty())
            {
                continue;
            }

            // Run recognition
            CtcResult ctc_result;
            cr = RecognizeTextCrop(
                ai_.Get(),
                rec_session_.Get(),
                rec_output_names_,
                rec_input_height_,
                rec_input_width_,
                crop,
                dict_,
                ctc_result);

            if (Das::IsFailed(cr))
            {
                DAS_CORE_LOG_WARN(
                    "Recognition failed for a text region: result={}",
                    cr);
                continue;
            }

            // Skip empty results
            if (ctc_result.text.empty())
            {
                continue;
            }

            // Build char boxes: distribute evenly across detection box
            // Use UTF-8 code point count (not byte count) for multi-byte
            // characters like Chinese (3 bytes per character in UTF-8)
            std::vector<Das::ExportInterface::DasRect> char_boxes;
            uint32_t                                   char_count = 0;
            {
                const auto& text = ctc_result.text;
                for (size_t i = 0; i < text.size();)
                {
                    unsigned char c = static_cast<unsigned char>(text[i]);
                    if (c < 0x80)
                    {
                        ++i;
                    }
                    else if ((c & 0xE0) == 0xC0)
                    {
                        i += 2;
                    }
                    else if ((c & 0xF0) == 0xE0)
                    {
                        i += 3;
                    }
                    else if ((c & 0xF8) == 0xF0)
                    {
                        i += 4;
                    }
                    else
                    {
                        ++i;
                    }
                    ++char_count;
                }
            }
            if (char_count > 0 && det_box.rect.width > 0)
            {
                int char_w = det_box.rect.width / static_cast<int>(char_count);
                for (uint32_t ci = 0; ci < char_count; ++ci)
                {
                    Das::ExportInterface::DasRect cr{};
                    cr.x = det_box.rect.x + static_cast<int>(ci) * char_w;
                    cr.y = det_box.rect.y;
                    cr.width = char_w;
                    cr.height = det_box.rect.height;
                    char_boxes.push_back(cr);
                }
            }

            // Create IDasOcrResultImpl
            Das::ExportInterface::DasRect line_box{};
            line_box.x = det_box.rect.x;
            line_box.y = det_box.rect.y;
            line_box.width = det_box.rect.width;
            line_box.height = det_box.rect.height;

            auto* ocr_result = new IDasOcrResultImpl(
                std::move(ctc_result.text),
                line_box,
                ctc_result.score,
                std::move(char_boxes),
                std::move(ctc_result.char_scores));
            ocr_result->AddRef();
            result_vector->AddResult(ocr_result);
            ocr_result->Release();
        }

        *pp_results = result_vector;
        return DAS_S_OK;
    }
    catch (const Ort::Exception& e)
    {
        DAS_CORE_LOG_ERROR("Recognize failed (ORT): {}", e.what());
        return DAS_E_ONNX_RUNTIME_ERROR;
    }
    catch (const std::bad_alloc&)
    {
        return DAS_E_OUT_OF_MEMORY;
    }
    catch (const std::exception& e)
    {
        DAS_CORE_LOG_ERROR("Recognize failed: {}", e.what());
        return DAS_E_FAIL;
    }
}

DAS_CORE_ORTWRAPPER_NS_END
