// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2015 Qianyi Zhou <Qianyi.Zhou@gmail.com>
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "Registration.h"

#include <cstdlib>
#include <ctime>

#include <Core/Utility/Console.h>
#include <Core/Geometry/PointCloud.h>
#include <Core/Geometry/KDTreeFlann.h>
#include <Core/Registration/Feature.h>

namespace three {

namespace {

RegistrationResult GetRegistrationResultAndCorrespondences(
		const PointCloud &source, const PointCloud &target,
		const KDTreeFlann &target_kdtree, double max_correspondence_distance,
		const Eigen::Matrix4d &transformation)
{
	RegistrationResult result(transformation);
	if (max_correspondence_distance <= 0.0) {
		return std::move(result);
	}

	double error2 = 0.0;

#ifdef _OPENMP
#pragma omp parallel
	{
#endif
		double error2_private = 0.0;
		CorrespondenceSet correspondence_set_private;
#ifdef _OPENMP
#pragma omp for nowait
#endif
		for (int i = 0; i < (int)source.points_.size(); i++) {
			std::vector<int> indices(1);
			std::vector<double> dists(1);
			const auto &point = source.points_[i];
			if (target_kdtree.SearchHybrid(point, max_correspondence_distance, 1,
					indices, dists) > 0) {
				error2_private += dists[0];
				correspondence_set_private.push_back(
						Eigen::Vector2i(i, indices[0]));
			}
		}
#ifdef _OPENMP
#pragma omp critical
#endif
		{
			for (int i = 0; i < correspondence_set_private.size(); i++) {
				result.correspondence_set_.push_back(
						correspondence_set_private[i]);
			}
			error2 += error2_private;
		}
#ifdef _OPENMP
	}
#endif

	if (result.correspondence_set_.empty()) {
		result.fitness_ = 0.0;
		result.inlier_rmse_ = 0.0;
	} else {
		size_t corres_number = result.correspondence_set_.size();
		result.fitness_ = (double)corres_number / (double)source.points_.size();
		result.inlier_rmse_ = std::sqrt(error2 / (double)corres_number);
	}
	return std::move(result);
}

RegistrationResult EvaluateRANSACBasedOnCorrespondence(const PointCloud &source,
		const PointCloud &target, const CorrespondenceSet &corres,
		double max_correspondence_distance,
		const Eigen::Matrix4d &transformation)
{
	RegistrationResult result(transformation);
	double error2 = 0.0;
	int good = 0;
	double max_dis2 = max_correspondence_distance * max_correspondence_distance;
	for (const auto &c : corres) {
		double dis2 =
				(source.points_[c[0]] - target.points_[c[1]]).squaredNorm();
		if (dis2 < max_dis2) {
			good++;
			error2 += dis2;
		}
	}
	if (good == 0) {
		result.fitness_ = 0.0;
		result.inlier_rmse_ = 0.0;
	} else {
		result.fitness_ = (double)good / (double)corres.size();
		result.inlier_rmse_ = std::sqrt(error2 / (double)good);
	}
	return result;
}

}	// unnamed namespace

RegistrationResult EvaluateRegistration(const PointCloud &source,
		const PointCloud &target, double max_correspondence_distance,
		const Eigen::Matrix4d &transformation/* = Eigen::Matrix4d::Identity()*/)
{
	KDTreeFlann kdtree;
	kdtree.SetGeometry(target);
	PointCloud pcd = source;
	if (transformation.isIdentity() == false) {
		pcd.Transform(transformation);
	}
	return GetRegistrationResultAndCorrespondences(pcd, target,
			kdtree, max_correspondence_distance, transformation);
}

RegistrationResult RegistrationICP(const PointCloud &source,
		const PointCloud &target, double max_correspondence_distance,
		const Eigen::Matrix4d &init/* = Eigen::Matrix4d::Identity()*/,
		const TransformationEstimation &estimation
		/* = TransformationEstimationPointToPoint(false)*/,
		const ICPConvergenceCriteria &criteria/* = ICPConvergenceCriteria()*/)
{
	if (max_correspondence_distance <= 0.0) {
		return RegistrationResult(init);
	}
	Eigen::Matrix4d transformation = init;
	KDTreeFlann kdtree;
	kdtree.SetGeometry(target);
	PointCloud pcd = source;
	if (init.isIdentity() == false) {
		pcd.Transform(init);
	}
	RegistrationResult result;
	result = GetRegistrationResultAndCorrespondences(
			pcd, target, kdtree, max_correspondence_distance, transformation);
	for (int i = 0; i < criteria.max_iteration_; i++) {
		PrintDebug("ICP Iteration #%d: Fitness %.4f, RMSE %.4f\n", i,
				result.fitness_, result.inlier_rmse_);
		Eigen::Matrix4d update = estimation.ComputeTransformation(
				pcd, target, result.correspondence_set_);
		transformation = update * transformation;
		pcd.Transform(update);
		RegistrationResult backup = result;
		result = GetRegistrationResultAndCorrespondences(pcd,
				target, kdtree, max_correspondence_distance, transformation);
		if (std::abs(backup.fitness_ - result.fitness_) <
				criteria.relative_fitness_ && std::abs(backup.inlier_rmse_ -
				result.inlier_rmse_) < criteria.relative_rmse_) {
			break;
		}
	}
	return result;
}

RegistrationResult RegistrationRANSACBasedOnCorrespondence(
		const PointCloud &source, const PointCloud &target,
		const CorrespondenceSet &corres, double max_correspondence_distance,
		const TransformationEstimation &estimation
		/* = TransformationEstimationPointToPoint(false)*/,
		int ransac_n/* = 6*/, const RANSACConvergenceCriteria &criteria
		/* = RANSACConvergenceCriteria()*/)
{
	if (ransac_n < 3 || (int)corres.size() < ransac_n ||
			max_correspondence_distance <= 0.0) {
		return RegistrationResult();
	}
	std::srand((unsigned int)std::time(0));
	Eigen::Matrix4d transformation;
	CorrespondenceSet ransac_corres(ransac_n);
	RegistrationResult result;
	for (int itr = 0; itr < criteria.max_iteration_ &&
			itr < criteria.max_validation_; itr++) {
		for (int j = 0; j < ransac_n; j++) {
			ransac_corres[j] = corres[std::rand() % (int)corres.size()];
		}
		transformation = estimation.ComputeTransformation(source,
				target, ransac_corres);
		PointCloud pcd = source;
		pcd.Transform(transformation);
		auto this_result = EvaluateRANSACBasedOnCorrespondence(pcd, target,
				corres, max_correspondence_distance, transformation);
		if (this_result.fitness_ > result.fitness_ ||
				(this_result.fitness_ == result.fitness_ &&
				this_result.inlier_rmse_ < result.inlier_rmse_)) {
			result = this_result;
		}
	}
	PrintDebug("RANSAC: Fitness %.4f, RMSE %.4f\n", result.fitness_,
			result.inlier_rmse_);
	return result;
}

RegistrationResult RegistrationRANSACBasedOnFeatureMatching(
		const PointCloud &source, const PointCloud &target,
		const Feature &source_feature, const Feature &target_feature,
		double max_correspondence_distance,
		const TransformationEstimation &estimation
		/* = TransformationEstimationPointToPoint(false)*/,
		int ransac_n/* = 4*/, const std::vector<std::reference_wrapper<const
		CorrespondenceChecker>> &checkers/* = {}*/,
		const RANSACConvergenceCriteria &criteria
		/* = RANSACConvergenceCriteria()*/)
{
	if (ransac_n < 3 || max_correspondence_distance <= 0.0) {
		return RegistrationResult();
	}

	RegistrationResult result;
	int total_validation = 0;
	bool finished_validation = false;
#ifdef _OPENMP
#pragma omp parallel
{
#endif
	CorrespondenceSet ransac_corres(ransac_n);
	KDTreeFlann kdtree(target);
	KDTreeFlann kdtree_feature(target_feature);
	RegistrationResult result_private;
	unsigned int seed_number;
#ifdef _OPENMP
		// each thread has different seed_number
	seed_number = (unsigned int)std::time(0) *
			(omp_get_thread_num() + 1);
#else
	seed_number = (unsigned int)std::time(0);
#endif
	std::srand(seed_number);

#ifdef _OPENMP
#pragma omp for nowait
#endif
	for (int itr = 0; itr < criteria.max_iteration_; itr++) {
		if (!finished_validation)
		{
			std::vector<int> indices(1);
			std::vector<double> dists(1);
			Eigen::Matrix4d transformation;
			for (int j = 0; j < ransac_n; j++) {
				ransac_corres[j](0) = std::rand() % (int)source.points_.size();
				if (kdtree_feature.SearchKNN(Eigen::VectorXd(
						source_feature.data_.col(ransac_corres[j](0))), 1,
						indices, dists) == 0) {
					PrintDebug("[RegistrationRANSACBasedOnFeatureMatching] Found a feature without neighbors.\n");
					ransac_corres[j](1) = 0;
				}
				else {
					ransac_corres[j](1) = indices[0];
				}
			}
			bool check = true;
			for (const auto &checker : checkers) {
				if (checker.get().require_pointcloud_alignment_ == false &&
						checker.get().Check(source, target, ransac_corres,
						transformation) == false) {
					check = false;
					break;
				}
			}
			if (check == false) continue;
			transformation = estimation.ComputeTransformation(source, target,
					ransac_corres);
			check = true;
			for (const auto &checker : checkers) {
				if (checker.get().require_pointcloud_alignment_ == true &&
						checker.get().Check(source, target, ransac_corres,
						transformation) == false) {
					check = false;
					break;
				}
			}
			if (check == false) continue;
			PointCloud pcd = source;
			pcd.Transform(transformation);
			auto this_result = GetRegistrationResultAndCorrespondences(
					pcd, target, kdtree, max_correspondence_distance,
					transformation);
			if (this_result.fitness_ > result_private.fitness_ ||
					(this_result.fitness_ == result_private.fitness_ &&
					this_result.inlier_rmse_ < result_private.inlier_rmse_)) {
				result_private = this_result;
			}
#ifdef _OPENMP
#pragma omp critical
			{
#endif
				total_validation++;
				if (total_validation >= criteria.max_validation_)
					finished_validation = true;
#ifdef _OPENMP
			}
#endif
		} // end of if statement
	} // end of for-loop
#ifdef _OPENMP
#pragma omp critical
	{
#endif
		if (result_private.fitness_ > result.fitness_ ||
			(result_private.fitness_ == result.fitness_ &&
				result_private.inlier_rmse_ < result.inlier_rmse_)) {
			result = result_private;
		}
#ifdef _OPENMP
	}
}
#endif
	PrintDebug("RANSAC: Fitness %.4f, RMSE %.4f\n", result.fitness_,
			result.inlier_rmse_);
	return result;
}

Eigen::Matrix6d GetInformationMatrixFromRegistrationResult(
		const PointCloud &source, const PointCloud &target,
		const RegistrationResult &result)
{
	// write q^*
	// see http://redwood-data.org/indoor/registration.html
	// note: I comes first and q_skew is scaled by factor 2.
	Eigen::Matrix6d GTG = Eigen::Matrix6d::Identity();
#ifdef _OPENMP
#pragma omp parallel
	{
#endif
		Eigen::Matrix6d GTG_private = Eigen::Matrix6d::Identity();
		Eigen::Vector6d G_r_private = Eigen::Vector6d::Zero();
#ifdef _OPENMP
#pragma omp parallel for schedule(static)
#endif
		for (auto c = 0; c < result.correspondence_set_.size(); c++) {
			int t = result.correspondence_set_[c](1);
			double x = target.points_[t](0);
			double y = target.points_[t](1);
			double z = target.points_[t](2);
			G_r_private.setZero();
			G_r_private(0) = 1.0;
			G_r_private(4) = 2.0 * z;
			G_r_private(5) = -2.0 * y;
			GTG_private.noalias() += G_r_private * G_r_private.transpose();
			G_r_private.setZero();
			G_r_private(1) = 1.0;
			G_r_private(3) = -2.0 * z;
			G_r_private(5) = 2.0 * x;
			GTG_private.noalias() += G_r_private * G_r_private.transpose();
			G_r_private.setZero();
			G_r_private(2) = 1.0;
			G_r_private(3) = 2.0 * y;
			G_r_private(4) = -2.0 * x;
			GTG_private.noalias() += G_r_private * G_r_private.transpose();
		}
#ifdef _OPENMP
#pragma omp critical
#endif
		{
			GTG += GTG_private;
		}
#ifdef _OPENMP
	}
#endif
	return std::move(GTG);
}

}	// namespace three