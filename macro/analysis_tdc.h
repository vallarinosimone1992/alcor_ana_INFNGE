#ifndef ANALYSIS_TDC_H
#define ANALYSIS_TDC_H

namespace analysis_tdc {

// ALCOR ToT pairs use even leading TDCs (0,2) and the adjacent odd trailing
// partner (1,3). Range validation is intentionally left to callers.
inline bool IsLeadingTdc(int tdc)
{
  return (tdc & 0x1) == 0;
}

inline bool IsTrailingTdc(int tdc)
{
  return (tdc & 0x1) == 1;
}

inline int GetTrailingPartner(int leading_tdc)
{
  return leading_tdc ^ 0x1;
}

inline int TdcPairIndex(int tdc)
{
  return tdc >> 1;
}

}  // namespace analysis_tdc

#endif
