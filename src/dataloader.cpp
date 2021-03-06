#include <iostream>

// 3rd party
#include "json/json.h"
#include "opencv2/imgproc/imgproc.hpp"

// own
#include "utils.h"
#include "dataloader.h"

namespace feh {

bool LoadEdgeMap(const std::string &filename, cv::Mat &edge) {
    vlslam_pb::EdgeMap edgemap;
    try {
        std::ifstream in_file(filename);
        edgemap.ParseFromIstream(&in_file);
        in_file.close();
        edge = cv::Mat(edgemap.rows(), edgemap.cols(),
                       CV_32FC1,
                       edgemap.mutable_data()->mutable_data());
        edge.convertTo(edge, CV_8UC1, 255.0f);
        return true;
    } catch (const std::exception &e) {
        return false;
    }
}


std::vector<std::string> LoadMeshDatabase(const std::string &root, const std::string &cat_json) {
    CHECK_STREQ(cat_json.substr(cat_json.find('.'), 5).c_str(), ".json");
    std::string content;
    std::string full_path = StrFormat("%d/%d", root, cat_json);
    auto json_content = LoadJson(full_path);

    std::vector<std::string> out;
    for (const auto &value : json_content["entries"]) {
        out.push_back(value.asString());
    }
    return out;
}

////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////
// HELPERS
////////////////////////////////////////////////////////////////////////////////
////////////////////////////////////////////////////////////////////////////////

VlslamDatasetLoader::VlslamDatasetLoader(const std::string &dataroot):
dataroot_(dataroot) {

    std::ifstream in_file(dataroot_ + "/dataset");
    CHECK(in_file.is_open()) << "failed to open dataset";

    dataset_.ParseFromIstream(&in_file);
    in_file.close();

    if (!Glob(dataroot_, ".png", png_files_)) {
        LOG(FATAL) << "FATAL::failed to read png file list @" << dataroot_;
    }

//    for (int i = 0; i < png_files_.size(); ++i) {
//        std::cout << png_files_[i] << "\n";
//    }


    if (!Glob(dataroot_, ".edge", edge_files_)) {
        LOG(FATAL) << "FATAL::failed to read edge map list @" << dataroot_;
    }

    if (!Glob(dataroot_, ".bbox", bbox_files_)) {
        LOG(FATAL) << "FATAL::failed to read bounding box lisst @" << dataroot_;
    }

    // CHECK_EQ(png_files_.size(), edge_files_.size());
    // CHECK_EQ(png_files_.size(), bbox_files_.size());
    size_ = png_files_.size();
}


bool VlslamDatasetLoader::Grab(int i,
                               cv::Mat &image,
                               cv::Mat &edgemap,
                               vlslam_pb::BoundingBoxList &bboxlist,
                               SE3f &gwc,
                               SO3f &Rg,
                               std::string &fullpath) {
    fullpath = png_files_[i];
    return Grab(i, image, edgemap, bboxlist, gwc, Rg);
}

bool VlslamDatasetLoader::Grab(int i,
                               cv::Mat &image,
                               cv::Mat &edgemap,
                               vlslam_pb::BoundingBoxList &bboxlist,
                               SE3f &gwc,
                               SO3f &Rg) {

    if (i >= size_ || i < 0) return false;
    std::cout << i << "\n";

    vlslam_pb::Packet *packet_ptr(dataset_.mutable_packets(i));
    gwc = SE3::from_matrix3x4(
          Eigen::Map<Eigen::Matrix<double, 3, 4, Eigen::RowMajor>>(
            packet_ptr->mutable_gwc()->mutable_data()));

    // gravity alignment rotation
    Vec3f Wg(packet_ptr->wg(0), packet_ptr->wg(1), 0);
    Rg = SO3f::exp(Wg);

    // read image
    std::string png_file = png_files_[i];
    image = cv::imread(png_file);
    // CHECK(!image.empty()) << "empty image: " << png_file;

    // read edge map if have any
    if (i < edge_files_.size()) {
        std::string edge_file = edge_files_[i];
        if (!LoadEdgeMap(edge_file, edgemap)) {
            LOG(FATAL) << "failed to load edge map @ " << edge_file;
        }
    }

    if (i < bbox_files_.size()) {
        std::string bbox_file = bbox_files_[i];
        // read bounding box
        std::ifstream in_file(bbox_file, std::ios::in);
        CHECK(in_file.is_open()) << "FATAL::failed to open bbox file @ " << bbox_file;
        bboxlist.ParseFromIstream(&in_file);
        in_file.close();
    }
    return true;
}


std::unordered_map<int64_t, std::array<ftype, 6>> VlslamDatasetLoader::GrabPointCloud(int i,
                                                                                  const cv::Mat &img) {
    std::unordered_map<int64_t, std::array<ftype, 6>> out;
    vlslam_pb::Packet *packet_ptr = dataset_.mutable_packets(i);
    for (auto f : packet_ptr->features()) {
        if (f.status() == vlslam_pb::Feature_Status_INSTATE
            || f.status() == vlslam_pb::Feature_Status_GOODDROP) {
            auto color = img.at<cv::Vec3b>(int(f.xp(1)), int(f.xp(0)));
            if (out.count(f.id())) {
                color[0] += out.at(f.id())[3];
                color[1] += out.at(f.id())[4];
                color[2] += out.at(f.id())[5];
                color[0] >>= 1;
                color[1] >>= 1;
                color[2] >>= 1;
                out[f.id()] = {f.xw(0), f.xw(1), f.xw(2),
                               static_cast<ftype>(color[0]),
                               static_cast<ftype>(color[1]),
                               static_cast<ftype>(color[2])};
            } else {
                out[f.id()] = {f.xw(0), f.xw(1), f.xw(2),
                               static_cast<ftype>(color[0]),
                               static_cast<ftype>(color[1]),
                               static_cast<ftype>(color[2])};
            }
        }
    }
    return out;
};

std::unordered_map<int64_t, std::array<ftype, 3>> VlslamDatasetLoader::GrabSparseDepth(int i) {
    std::unordered_map<int64_t, std::array<ftype, 3>> out;
    vlslam_pb::Packet *packet_ptr = dataset_.mutable_packets(i);

    auto gwc = SE3::from_matrix3x4(
        Eigen::Map<Eigen::Matrix<double, 3, 4, Eigen::RowMajor>>(
          packet_ptr->mutable_gwc()->mutable_data()));
    auto gcw = gwc.inv();


    for (auto f : packet_ptr->features()) {
        if (f.status() == vlslam_pb::Feature_Status_INSTATE
            || f.status() == vlslam_pb::Feature_Status_GOODDROP) {


            Vec3f Xw{f.xw(0), f.xw(1), f.xw(2)};
            // Vec3f Xc = gcw.rotationMatrix() * Xw + gcw.translation();
            Vec3f Xc = gcw * Xw;
            float z = Xc[2];

            if (out.count(f.id())) {
                out[f.id()] = {f.xp(0), f.xp(1), z};
            } else {
                out[f.id()] = {f.xp(0), f.xp(1), z};
            }
        }
    }
    return out;
};




}   // namespace feh
