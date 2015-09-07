
// Copyright (c) 2015 Pierre MOULON.

// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at http://mozilla.org/MPL/2.0/.

#include "openMVG/sfm/pipelines/localization/SfM_Localizer_Single_3DTrackObservation_Database.hpp"
#include "openMVG/matching/indMatch.hpp"
#include "openMVG/sfm/pipelines/sfm_robust_model_estimation.hpp"

namespace openMVG {
namespace sfm {

  SfM_Localization_Single_3DTrackObservation_Database::
  SfM_Localization_Single_3DTrackObservation_Database()
  :SfM_Localizer(), sfm_data_(nullptr)
  {}

  bool
  SfM_Localization_Single_3DTrackObservation_Database::Init
  (
    const SfM_Data & sfm_data,
    const Regions_Provider & regions_provider
  )
  {
    if (regions_provider.regions_per_view.empty())
    {
      return false;
    }

    if (sfm_data.GetPoses().empty() || sfm_data.GetLandmarks().empty())
    {
      std::cerr << std::endl
        << "The input SfM_Data file have not 3D content to match with." << std::endl;
      return false;
    }

    // Setup the database
    // A collection of regions
    // -each view observation leads to a new regions
    // - link each observation region to a track id to ease 2D-3D correspondences search

    const features::Regions * regions_type = std::begin(regions_provider.regions_per_view)->second.get();
    landmark_observations_descriptors_.reset(regions_type->EmptyClone());
    for (const auto & landmarks : sfm_data.GetLandmarks())
    {
      for (const auto & landmark : landmarks.second.obs)
      {
        if (landmark.second.id_feat != UndefinedIndexT)
        {
          // copy the feature/descriptor to landmark_observations_descriptors
          const features::Regions * view_regions = regions_provider.regions_per_view.at(landmark.first).get();
          view_regions->CopyRegion(landmark.second.id_feat, landmark_observations_descriptors_.get());
          // link this descriptor to the track Id
          index_to_landmark_id_.push_back(landmarks.first);
        }
      }
    }
    std::cout << "Init retrieval database ... " << std::endl;
    matching_interface_ =
      matching::Matcher_Regions_Database(matching::ANN_L2, *landmark_observations_descriptors_.get());
    std::cout << "Retrieval database initialized\n"
      << "#landmark: " << sfm_data.GetLandmarks().size() << "\n"
      << "#descriptor initialized: " << landmark_observations_descriptors_->RegionCount() << std::endl;

    sfm_data_ = &sfm_data;

    return true;
  }

  bool
  SfM_Localization_Single_3DTrackObservation_Database::Localize
  (
    const Pair & image_size,
    const cameras::IntrinsicBase * optional_intrinsics,
    const features::Regions & query_regions,
    geometry::Pose3 & pose,
    Image_Localizer_Match_Data * resection_data_ptr
  ) const
  {
    if (sfm_data_ == nullptr)
    {
      return false;
    }

    matching::IndMatches vec_putative_matches;
    if (!matching_interface_.Match(0.8, query_regions, vec_putative_matches))
    {
      return false;
    }

    std::cout << "#3D2d putative correspondences: " << vec_putative_matches.size() << std::endl;
    // Init the 3D-2d correspondences array
    Image_Localizer_Match_Data resection_data;
    resection_data.pt3D.resize(3, vec_putative_matches.size());
    resection_data.pt2D.resize(2, vec_putative_matches.size());
    Mat2X pt2D_original(2, vec_putative_matches.size());
    for (size_t i = 0; i < vec_putative_matches.size(); ++i)
    {
      resection_data.pt3D.col(i) = sfm_data_->GetLandmarks().at(index_to_landmark_id_[vec_putative_matches[i]._i]).X;
      resection_data.pt2D.col(i) = query_regions.GetRegionPosition(vec_putative_matches[i]._j);
      pt2D_original.col(i) = resection_data.pt2D.col(i);
      // Handle image distortion if intrinsic is known (to ease the resection)
      if (optional_intrinsics && optional_intrinsics->have_disto())
      {
        resection_data.pt2D.col(i) = optional_intrinsics->get_ud_pixel(resection_data.pt2D.col(i));
      }
    }

    // --
    // Compute the camera pose (resectioning)
    // --
    double errorMax = std::numeric_limits<double>::max();
    Mat34 P;
    resection_data.vec_inliers.clear();

    const cameras::Pinhole_Intrinsic * pinhole_cam = dynamic_cast<const cameras::Pinhole_Intrinsic *>(optional_intrinsics);
    const bool bResection = sfm::robustResection(
      image_size,
      resection_data.pt2D, resection_data.pt3D,
      &resection_data.vec_inliers,
      (pinhole_cam == nullptr) ? nullptr : &pinhole_cam->K(),
      &P, &errorMax);

    if (bResection)
    {
      resection_data.projection_matrix = P;
      Mat3 K, R;
      Vec3 t;
      KRt_From_P(P, &K, &R, &t);
      pose = geometry::Pose3(R, -R.transpose() * t);
    }
    resection_data.pt2D = std::move(pt2D_original); // restore original image domain points

    std::cout << std::endl
      << "-------------------------------" << "\n"
      << "-- Robust Resection " << "\n"
      << "-- Resection status: " << bResection << "\n"
      << "-- #Points used for Resection: " << vec_putative_matches.size() << "\n"
      << "-- #Points validated by robust Resection: " << resection_data.vec_inliers.size() << "\n"
      << "-- Threshold: " << errorMax << "\n"
      << "-------------------------------" << std::endl;

    if (resection_data_ptr != nullptr)
      (*resection_data_ptr) = std::move(resection_data);

    return bResection;
  }

} // namespace sfm
} // namespace openMVG
