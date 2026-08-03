#include "hardware/cherry/cherry_serial_sensor.h"

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

std::string make_temp_path()
{
    char path[] = "/tmp/cherry-json-XXXXXX";
    const int fd = mkstemp(path);
    assert(fd >= 0);
    assert(close(fd) == 0);
    return path;
}

void assert_python_json(const std::string& path, const std::string& expression)
{
    const std::string command =
        "python3 -c 'import json,sys; d=json.load(open(sys.argv[1])); assert " +
        expression + "' '" + path + "'";
    assert(std::system(command.c_str()) == 0);
}

void test_imu_json_preserves_batch_and_signed_samples()
{
    const cherry::ImuFrame frame{
        17,
        1234567890123456ULL,
        1234567890999999ULL,
        {{-32768, 1234, -56, -123456789, 9223372036854775808ULL}},
        {{32767, -2, 3, 25000, 18446744073709551614ULL},
         {-4, 5, -6, -42, 9876543210123456ULL}},
    };

    const std::string path = make_temp_path();
    FILE* file = fopen(path.c_str(), "wb");
    assert(file != nullptr);
    assert(cherry::write_imu_jsonl(file, frame));
    assert(fclose(file) == 0);

    assert_python_json(
        path,
        "d[\"generation\"]==17 and "
        "d[\"window_begin_pts_us\"]==1234567890123456 and "
        "d[\"window_end_pts_us\"]==1234567890999999 and "
        "len(d[\"gyro_samples\"])==1 and len(d[\"acc_samples\"])==2 and "
        "d[\"gyro_samples\"][0]=={\"x\":-32768,\"y\":1234,\"z\":-56,"
        "\"temperature\":-123456789,\"pts_us\":9223372036854775808} and "
        "d[\"acc_samples\"][0][\"pts_us\"]==18446744073709551614 and "
        "d[\"acc_samples\"][1][\"temperature\"]==-42");
    assert(unlink(path.c_str()) == 0);
}

void test_mag_json_preserves_raw_values_and_timestamp()
{
    const cherry::MagFrame frame{
        23,
        {{-2147483647, 527700, -523800, 255, 12345678901234567ULL},
         {9, -8, 7, 0, 18446744073709551613ULL}},
    };

    const std::string path = make_temp_path();
    FILE* file = fopen(path.c_str(), "wb");
    assert(file != nullptr);
    assert(cherry::write_mag_jsonl(file, frame));
    assert(fclose(file) == 0);

    assert_python_json(
        path,
        "d[\"generation\"]==23 and len(d[\"samples\"])==2 and "
        "d[\"samples\"][0]=={\"x_raw\":-2147483647,\"y_raw\":527700,"
        "\"z_raw\":-523800,\"tout_raw\":255,"
        "\"pts_us\":12345678901234567} and "
        "d[\"samples\"][1][\"pts_us\"]==18446744073709551613");
    assert(unlink(path.c_str()) == 0);
}

void test_frame_meta_json_preserves_both_sensor_records()
{
    const cherry::FrameMeta frame{
        29,
        {{0, 7, 4000000000U, 123456789012345678ULL},
         {1, 8, 42, 18446744073709551612ULL}},
    };

    const std::string path = make_temp_path();
    FILE* file = fopen(path.c_str(), "wb");
    assert(file != nullptr);
    assert(cherry::write_frame_meta_jsonl(file, frame));
    assert(fclose(file) == 0);

    assert_python_json(
        path,
        "d[\"generation\"]==29 and len(d[\"samples\"])==2 and "
        "d[\"samples\"][0]=={\"sensor_idx\":0,\"vi_pipe\":7,"
        "\"frame_id\":4000000000,\"frame_pts_us\":123456789012345678} and "
        "d[\"samples\"][1][\"sensor_idx\"]==1 and "
        "d[\"samples\"][1][\"frame_pts_us\"]==18446744073709551612");
    assert(unlink(path.c_str()) == 0);
}

} // namespace

int main()
{
    test_imu_json_preserves_batch_and_signed_samples();
    test_mag_json_preserves_raw_values_and_timestamp();
    test_frame_meta_json_preserves_both_sensor_records();
    return 0;
}
