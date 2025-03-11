#include <scfg.h>
#include <string.h>

#include "cfg.h"

static bool
parse_multi_output(const struct scfg_directive *dir, enum cg_multi_output_mode *output_mode)
{
	if (dir->params_len != 1) {
		wlr_log(WLR_ERROR, "'multi-output' expects exactly one parameter");
		return false;
	}

	if (strcmp(dir->params[0], "last") == 0) {
		*output_mode = CAGE_MULTI_OUTPUT_MODE_LAST;
	} else if (strcmp(dir->params[0], "extend") == 0) {
		*output_mode = CAGE_MULTI_OUTPUT_MODE_EXTEND;
	} else {
		wlr_log(WLR_ERROR, "Unknown parameter '%s' for 'multi-output'", dir->params[0]);
		return false;
	}

	return true;
}

static bool
parse_boolean(const struct scfg_directive *dir, bool *value)
{
	if (dir->params_len != 1) {
		wlr_log(WLR_ERROR, "'%s' expects exactly one parameter\n", dir->name);
		return false;
	}

	if (strcmp(dir->params[0], "true") == 0) {
		*value = true;
	} else if (strcmp(dir->params[0], "false") == 0) {
		*value = false;
	} else {
		wlr_log(WLR_ERROR, "Unknown parameter '%s' for boolean '%s'", dir->params[0], dir->name);
		return false;
	}

	return true;
}

bool
parse_config_file(const char *path, struct cg_server *server)
{
	bool parsed = false;
	struct scfg_block block = {0};

	if (scfg_load_file(&block, path) != 0) {
		wlr_log(WLR_ERROR, "Failed to parse config file: %s\n", path);
		return false;
	}

	for (size_t i = 0; i < block.directives_len; i++) {
		struct scfg_directive *dir = &block.directives[i];

		if (strcmp(dir->name, "multi-output") == 0) {
			parsed = parse_multi_output(dir, &server->output_mode);
		} else if (strcmp(dir->name, "xdg-decoration") == 0) {
			parsed = parse_boolean(dir, &server->xdg_decoration);
		} else if (strcmp(dir->name, "vt-switch") == 0) {
			parsed = parse_boolean(dir, &server->allow_vt_switch);
		} else {
			wlr_log(WLR_ERROR, "Unknown option '%s'", dir->name);
			parsed = false;
		}

		if (!parsed) {
			break;
		}
	}

	scfg_block_finish(&block);
	return parsed;
}