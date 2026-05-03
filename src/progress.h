#pragma once

#include <string>

namespace lathe {

void progress_start(const std::string& input,
                    const std::string& output,
                    double duration_s);
void progress_update(double percent, double time_s, const std::string& speed);
void progress_done(const std::string& output);
void progress_cancelled();
void progress_error(const std::string& message);

}
