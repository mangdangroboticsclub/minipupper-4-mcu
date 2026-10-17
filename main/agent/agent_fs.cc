#include "agent/agent_fs.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_littlefs.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "claw_paths.h"

namespace agent_fs {

namespace {

const char *TAG = "agent_fs";

constexpr const char *kSystemPartition = "system";
constexpr const char *kRecoveryDir     = "/system/.recovery";
constexpr const char *kDataRoot        = CONFIG_APP_AGENT_DATA_DIR;

/* The folders app_claw_start() expects to find. Most are created by the
 * module that owns them, but a missing memory/ or router_rules/ is fatal to
 * startup, and making them all here is cheap. */
const char *const kDataDirs[] = {
	"sessions", "memory", "skills", "scripts", "router_rules", "scheduler", "inbox",
};

/* Paths are at most "/fs/agent/" + a recovery-relative path: 96 is what
 * ESP-Claw itself allows for a file path (APP_CLAW_FILE_PATH_LEN). */
constexpr size_t kPathMax = 96;

bool s_ready = false;
bool s_tried = false;
esp_err_t s_result = ESP_FAIL;

bool path_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}

bool make_dir(const char *path)
{
	if (mkdir(path, 0777) == 0 || errno == EEXIST) {
		return true;
	}
	ESP_LOGE(TAG, "mkdir %s: %s", path, strerror(errno));
	return false;
}

bool join(char *out, size_t out_size, const char *dir, const char *name)
{
	const int n = snprintf(out, out_size, "%s/%s", dir, name);
	return n > 0 && static_cast<size_t>(n) < out_size;
}

esp_err_t copy_file(const char *src_path, const char *dst_path)
{
	FILE *src = fopen(src_path, "rb");
	if (!src) {
		ESP_LOGE(TAG, "open %s: %s", src_path, strerror(errno));
		return ESP_FAIL;
	}
	FILE *dst = fopen(dst_path, "wb");
	if (!dst) {
		ESP_LOGE(TAG, "create %s: %s", dst_path, strerror(errno));
		fclose(src);
		return ESP_FAIL;
	}

	/* 512 bytes: the seeds are a few KB each and this runs once per boot
	 * from the main task, whose stack is sized for it (sdkconfig). */
	char buf[512];
	esp_err_t ret = ESP_OK;
	size_t n;
	while ((n = fread(buf, 1, sizeof(buf), src)) > 0) {
		if (fwrite(buf, 1, n, dst) != n) {
			ESP_LOGE(TAG, "write %s: %s", dst_path, strerror(errno));
			ret = ESP_FAIL;
			break;
		}
	}
	if (ret == ESP_OK && ferror(src)) {
		ESP_LOGE(TAG, "read %s failed", src_path);
		ret = ESP_FAIL;
	}
	fclose(dst);
	fclose(src);
	if (ret != ESP_OK) {
		unlink(dst_path);
	}
	return ret;
}

/* Copy every file under src_dir that does not exist under dst_dir, one
 * directory level at a time (the seeds are two levels deep). Existing files
 * are never touched: the owner's memory and rules are theirs. */
esp_err_t recover_missing(const char *src_dir, const char *dst_dir)
{
	DIR *dir = opendir(src_dir);
	if (!dir) {
		ESP_LOGW(TAG, "no recovery seeds at %s (%s)", src_dir, strerror(errno));
		return ESP_OK;
	}

	esp_err_t result = ESP_OK;
	struct dirent *entry;
	while ((entry = readdir(dir)) != nullptr) {
		if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
			continue;
		}
		char src[kPathMax];
		char dst[kPathMax];
		if (!join(src, sizeof(src), src_dir, entry->d_name) ||
		    !join(dst, sizeof(dst), dst_dir, entry->d_name)) {
			ESP_LOGW(TAG, "path too long under %s: %s", src_dir, entry->d_name);
			result = ESP_ERR_INVALID_SIZE;
			continue;
		}

		struct stat st;
		if (stat(src, &st) != 0) {
			continue;
		}
		if (S_ISDIR(st.st_mode)) {
			if (!make_dir(dst)) {
				result = ESP_FAIL;
				continue;
			}
			const esp_err_t sub = recover_missing(src, dst);
			if (sub != ESP_OK) {
				result = sub;
			}
			continue;
		}
		if (path_exists(dst)) {
			continue;
		}
		ESP_LOGI(TAG, "seeding %s", dst);
		if (copy_file(src, dst) != ESP_OK) {
			result = ESP_FAIL;
		}
	}
	closedir(dir);
	return result;
}

esp_err_t mount_system()
{
	esp_vfs_littlefs_conf_t conf = {};
	conf.base_path = kSystemRoot;
	conf.partition_label = kSystemPartition;
	conf.format_if_mount_failed = false;   // read-only image: never format it
	conf.read_only = true;
	conf.dont_mount = false;

	const esp_err_t ret = esp_vfs_littlefs_register(&conf);
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "mount %s (%s): %s -- was the `system` partition flashed?",
		         kSystemRoot, kSystemPartition, esp_err_to_name(ret));
		return ret;
	}
	size_t total = 0;
	size_t used = 0;
	if (esp_littlefs_info(kSystemPartition, &total, &used) == ESP_OK) {
		ESP_LOGI(TAG, "%s mounted: %u of %u KB used", kSystemRoot,
		         (unsigned)(used / 1024), (unsigned)(total / 1024));
	}
	return ESP_OK;
}

esp_err_t init_once()
{
	esp_err_t ret = mount_system();
	if (ret != ESP_OK) {
		return ret;
	}

	if (!make_dir(kDataRoot)) {
		return ESP_FAIL;
	}
	for (const char *sub : kDataDirs) {
		char path[kPathMax];
		if (!join(path, sizeof(path), kDataRoot, sub) || !make_dir(path)) {
			return ESP_FAIL;
		}
	}

	ret = recover_missing(kRecoveryDir, kDataRoot);
	if (ret != ESP_OK) {
		ESP_LOGW(TAG, "recovery into %s incomplete: %s", kDataRoot, esp_err_to_name(ret));
	}

	ret = claw_paths_set(CLAW_PATH_DATA, kDataRoot);
	if (ret == ESP_OK) {
		ret = claw_paths_set(CLAW_PATH_SYSTEM, kSystemRoot);
	}
	if (ret != ESP_OK) {
		ESP_LOGE(TAG, "claw_paths_set: %s", esp_err_to_name(ret));
		return ret;
	}
	ESP_LOGI(TAG, "agent roots: data=%s system=%s", kDataRoot, kSystemRoot);
	return ESP_OK;
}

}  // namespace

esp_err_t init()
{
	if (s_tried) {
		return s_result;
	}
	s_tried = true;
	s_result = init_once();
	s_ready = (s_result == ESP_OK);
	return s_result;
}

bool ready()
{
	return s_ready;
}

}  // namespace agent_fs
