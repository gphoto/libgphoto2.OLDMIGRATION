int 	gp_init(int debug);
int	gp_is_initialized(void);
int 	gp_exit();

void 	gp_debug_printf(int level, char *id, char *format, ...);

int 	gp_frontend_register(CameraStatus status, CameraProgress progress, 
			     CameraMessage message, CameraConfirm confirm,
			     CameraPrompt prompt);

char   *gp_result_as_string (int result);
