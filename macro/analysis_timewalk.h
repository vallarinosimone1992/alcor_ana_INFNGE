#ifndef ANALYSIS_TIMEWALK_H
#define ANALYSIS_TIMEWALK_H

#include <iostream>
#include <string>

namespace analysis_timewalk {

// Numeric values are the persistent ROOT schema for timewalk_corr_model_ch*.
// Keep them stable so older calibration files remain readable.
enum class TimewalkFitModel {
  Pol1 = 0,
  LinExpPlateau = 1,
  Pol1Plateau = 2,
  InversePower = 3,
};

static_assert(static_cast<int>(TimewalkFitModel::Pol1) == 0,
              "TimewalkFitModel::Pol1 ROOT id changed; this breaks persisted calibration compatibility");
static_assert(static_cast<int>(TimewalkFitModel::LinExpPlateau) == 1,
              "TimewalkFitModel::LinExpPlateau ROOT id changed; this breaks persisted calibration compatibility");
static_assert(static_cast<int>(TimewalkFitModel::Pol1Plateau) == 2,
              "TimewalkFitModel::Pol1Plateau ROOT id changed; this breaks persisted calibration compatibility");
static_assert(static_cast<int>(TimewalkFitModel::InversePower) == 3,
              "TimewalkFitModel::InversePower ROOT id changed; this breaks persisted calibration compatibility");

inline int TimewalkFitModelId(TimewalkFitModel model)
{
  return static_cast<int>(model);
}

inline TimewalkFitModel TimewalkFitModelFromId(int model_id, bool *known = nullptr)
{
  if (known) {
    *known = true;
  }
  switch (model_id) {
    case 0:
      return TimewalkFitModel::Pol1;
    case 1:
      return TimewalkFitModel::LinExpPlateau;
    case 2:
      return TimewalkFitModel::Pol1Plateau;
    case 3:
      return TimewalkFitModel::InversePower;
    default:
      if (known) {
        *known = false;
      }
      return TimewalkFitModel::Pol1;
  }
}

inline std::string TimewalkFitModelName(TimewalkFitModel model)
{
  switch (model) {
    case TimewalkFitModel::Pol1:
      return "pol1";
    case TimewalkFitModel::LinExpPlateau:
      return "lin-exp-plateau";
    case TimewalkFitModel::Pol1Plateau:
      return "pol1-plateau";
    case TimewalkFitModel::InversePower:
      return "inverse-power";
  }
  return "unknown";
}

inline TimewalkFitModel ParseTimewalkFitModel(const std::string &value)
{
  if (value == "pol1" || value == "linear") {
    return TimewalkFitModel::Pol1;
  }
  if (value == "lin-exp-plateau" || value == "lin_exp_plateau" || value == "piecewise") {
    return TimewalkFitModel::LinExpPlateau;
  }
  if (value == "pol1-plateau" || value == "pol1_plateau" || value == "linear-plateau" ||
      value == "linear_plateau" || value == "piecewise-linear" || value == "piecewise_linear") {
    return TimewalkFitModel::Pol1Plateau;
  }
  if (value == "inverse-power" || value == "inverse_power" || value == "power" || value == "threshold-power" ||
      value == "threshold_power") {
    return TimewalkFitModel::InversePower;
  }
  std::cerr << "Unknown timewalk fit model '" << value << "', using inverse-power" << std::endl;
  return TimewalkFitModel::InversePower;
}

}  // namespace analysis_timewalk

#endif
