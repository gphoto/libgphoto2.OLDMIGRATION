int camera_id 			(CameraText *id);
int camera_abilities 		(CameraAbilitiesList *list);
int camera_init 		(Camera *camera);
int camera_exit 		(Camera *camera);

int camera_get_config		(Camera *camera, CameraWidget **window);
int camera_set_config		(Camera *camera, CameraWidget  *window);
int camera_capture 		(Camera *camera, int capture_type, CameraFilePath *path);
int camera_capture_preview     	(Camera *camera, CameraFile *file);
int camera_summary 		(Camera *camera, CameraText *summary);
int camera_manual 		(Camera *camera, CameraText *manual);
int camera_about 		(Camera *camera, CameraText *about);

int camera_folder_list_folders	(Camera *camera, char *folder, CameraList *list);
int camera_folder_list_files   	(Camera *camera, char *folder, CameraList *list);
int camera_folder_put_file     	(Camera *camera, char *folder, CameraFile *file);
int camera_folder_delete_all   	(Camera *camera, char *folder);
int camera_folder_get_config	(Camera *camera, char *folder, CameraWidget **window);
int camera_folder_set_config	(Camera *camera, char *folder, CameraWidget  *window);

int camera_file_get_info        (Camera *camera, char *folder, char *filename, CameraFileInfo *info);
int camera_file_set_info	(Camera *camera, char *folder, char *filename, CameraFileInfo *info);
int camera_file_get       	(Camera *camera, char *folder, char *filename, CameraFile *file);
int camera_file_get_preview	(Camera *camera, char *folder, char *filename, CameraFile *file);
int camera_file_delete   	(Camera *camera, char *folder, char *filename);
int camera_file_get_config	(Camera *camera, char *folder, char *filename, CameraWidget **window);
int camera_file_set_config	(Camera *camera, char *folder, char *filename, CameraWidget  *window);

char *camera_result_as_string 	(Camera *camera, int result);
