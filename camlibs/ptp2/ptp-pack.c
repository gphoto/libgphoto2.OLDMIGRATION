/* currently this file is included into ptp.c */

#include <iconv.h>

static inline uint16_t
htod16p (PTPParams *params, uint16_t var)
{
	return ((params->byteorder==PTP_DL_LE)?htole16(var):htobe16(var));
}

static inline uint32_t
htod32p (PTPParams *params, uint32_t var)
{
	return ((params->byteorder==PTP_DL_LE)?htole32(var):htobe32(var));
}

static inline void
htod16ap (PTPParams *params, unsigned char *a, uint16_t val)
{
	if (params->byteorder==PTP_DL_LE)
		htole16a(a,val);
	else 
		htobe16a(a,val);
}

static inline void
htod32ap (PTPParams *params, unsigned char *a, uint32_t val)
{
	if (params->byteorder==PTP_DL_LE)
		htole32a(a,val);
	else 
		htobe32a(a,val);
}

static inline uint16_t
dtoh16p (PTPParams *params, uint16_t var)
{
	return ((params->byteorder==PTP_DL_LE)?le16toh(var):be16toh(var));
}

static inline uint32_t
dtoh32p (PTPParams *params, uint32_t var)
{
	return ((params->byteorder==PTP_DL_LE)?le32toh(var):be32toh(var));
}

static inline uint16_t
dtoh16ap (PTPParams *params, unsigned char *a)
{
	return ((params->byteorder==PTP_DL_LE)?le16atoh(a):be16atoh(a));
}

static inline uint32_t
dtoh32ap (PTPParams *params, unsigned char *a)
{
	return ((params->byteorder==PTP_DL_LE)?le32atoh(a):be32atoh(a));
}

static inline uint64_t
dtoh64ap (PTPParams *params, unsigned char *a)
{
	uint64_t tmp = 0;
	int i;

	if (params->byteorder==PTP_DL_LE) {
		for (i=0;i<8;i++)
			tmp |= (((uint64_t)a[i]) << (8*i));
	} else {
		for (i=0;i<8;i++)
			tmp |= (((uint64_t)a[i]) << (8*(7-i)));
	}
	return tmp;
}

#define htod8a(a,x)	*(uint8_t*)(a) = x
#define htod16a(a,x)	htod16ap(params,a,x)
#define htod32a(a,x)	htod32ap(params,a,x)
#define htod16(x)	htod16p(params,x)
#define htod32(x)	htod32p(params,x)

#define dtoh8a(x)	(*(uint8_t*)(x))
#define dtoh16a(a)	dtoh16ap(params,a)
#define dtoh32a(a)	dtoh32ap(params,a)
#define dtoh64a(a)	dtoh64ap(params,a)
#define dtoh16(x)	dtoh16p(params,x)
#define dtoh32(x)	dtoh32p(params,x)


static inline char*
ptp_unpack_string(PTPParams *params, unsigned char* data, uint16_t offset, uint8_t *len)
{
	int i;
	uint8_t loclen;

	/* Cannot exceed 255 (PTP_MAXSTRLEN) since it is a single byte, duh ... */
	loclen = dtoh8a(&data[offset]);
	/* This len is used to advance the buffer pointer */
	*len = loclen;
	if (loclen) {
		uint16_t string[PTP_MAXSTRLEN+1];
		char *stringp = (char *) string;
		char loclstr[PTP_MAXSTRLEN*3+1]; /* UTF-8 encoding is max 3 bytes per UCS2 char. */
		char *locp = loclstr;
		size_t nconv;
		size_t convlen = loclen * 2; /* UCS-2 is 16 bit wide */
		size_t convmax = PTP_MAXSTRLEN*3;
		
		for (i=0;i<loclen;i++) {
			string[i]=dtoh16a(&data[offset+i*2+1]);
		}
		/* be paranoid! Add a terminator. :( */
		string[loclen]=0x0000U;
		loclstr[0]='\0';
		/* loclstr=ucs2_to_utf8(string); */
		/* Do the conversion.  */
		nconv = iconv (params->cd_ucs2_to_locale, &stringp, &convlen, &locp, &convmax);
		/* FIXME: handle size errors */
		loclstr[PTP_MAXSTRLEN*3] = '\0';
		if (nconv == (size_t) -1)
			return NULL;
		return strdup(loclstr);
	}
	return NULL;
}


static inline int
ucs2strlen(uint16_t const * const unicstr)
{
	int length;
	
	/* Unicode strings are terminated with 2 * 0x00 */
	for(length = 0; unicstr[length] != 0x0000U; length ++);
	return length;
}


static inline void
ptp_pack_string(PTPParams *params, char *string, unsigned char* data, uint16_t offset, uint8_t *len)
{
	int i;
	int packedlen;
	uint16_t ucs2str[PTP_MAXSTRLEN+1];
	char *ucs2strp = (char *) ucs2str;
	char *stringp = string;
	size_t nconv;
	size_t convlen = strlen(string);
	size_t convmax = PTP_MAXSTRLEN * 2; /* Includes the terminator */

	/* Cannot exceed 255 (PTP_MAXSTRLEN) since it is a single byte, duh ... */
	ucs2str[0] = 0x0000U;
	memset(ucs2strp, 0, PTP_MAXSTRLEN*2+2);
	nconv = iconv (params->cd_locale_to_ucs2, &stringp, &convlen, &ucs2strp, &convmax);
	if (nconv == (size_t) -1)
		ucs2str[0] = 0x0000U;
	packedlen = ucs2strlen(ucs2str);
	if (packedlen > PTP_MAXSTRLEN-1) {
		*len=0;
		return;
	}
	
	/* number of characters including terminating 0 (PTP standard confirmed) */
	htod8a(&data[offset],packedlen+1);
	for (i=0;i<packedlen && i< PTP_MAXSTRLEN; i++) {
		htod16a(&data[offset+i*2+1],ucs2str[i]);
	}
	htod16a(&data[offset+i*2+1],0x0000);

	/* The returned length is in number of characters */
	*len = (uint8_t) packedlen+1;
}

static inline unsigned char *
ptp_get_packed_stringcopy(PTPParams *params, char *string, uint32_t *packed_size)
{
	uint8_t packed[PTP_MAXSTRLEN*2+3], len;
	size_t plen;
	unsigned char *retcopy = NULL;

	ptp_pack_string(params, string, (unsigned char*) packed, 0, &len);
	/* returned length is in characters, then one byte for string length */
	plen = len*2 + 1;
	
	retcopy = malloc(plen);
	if (!retcopy) {
		*packed_size = 0;
		return NULL;
	}
	memcpy(retcopy, packed, plen);
	*packed_size = plen;
	return (retcopy);
}

static inline uint32_t
ptp_unpack_uint32_t_array(PTPParams *params, unsigned char* data, uint16_t offset, uint32_t **array)
{
	uint32_t n, i=0;

	n=dtoh32a(&data[offset]);
	*array = malloc (n*sizeof(uint32_t));
	while (n>i) {
		(*array)[i]=dtoh32a(&data[offset+(sizeof(uint32_t)*(i+1))]);
		i++;
	}
	return n;
}

static inline uint32_t
ptp_pack_uint32_t_array(PTPParams *params, uint32_t *array, uint32_t arraylen, unsigned char **data )
{
	uint32_t i=0;

	*data = malloc ((arraylen+1)*sizeof(uint32_t));
	htod32a(&(*data)[0],arraylen);
	for (i=0;i<arraylen;i++)
		htod32a(&(*data)[sizeof(uint32_t)*(i+1)], array[i]);
	return (arraylen+1)*sizeof(uint32_t);
}

static inline uint32_t
ptp_unpack_uint16_t_array(PTPParams *params, unsigned char* data, uint16_t offset, uint16_t **array)
{
	uint32_t n, i=0;

	n=dtoh32a(&data[offset]);
	*array = malloc (n*sizeof(uint16_t));
	while (n>i) {
		(*array)[i]=dtoh16a(&data[offset+(sizeof(uint16_t)*(i+2))]);
		i++;
	}
	return n;
}

/* DeviceInfo pack/unpack */

#define PTP_di_StandardVersion		 0
#define PTP_di_VendorExtensionID	 2
#define PTP_di_VendorExtensionVersion	 6
#define PTP_di_VendorExtensionDesc	 8
#define PTP_di_FunctionalMode		 8
#define PTP_di_OperationsSupported	10

static inline void
ptp_unpack_DI (PTPParams *params, unsigned char* data, PTPDeviceInfo *di, unsigned int datalen)
{
	uint8_t len;
	unsigned int totallen;
	
	di->StandardVersion = dtoh16a(&data[PTP_di_StandardVersion]);
	di->VendorExtensionID =
		dtoh32a(&data[PTP_di_VendorExtensionID]);
	di->VendorExtensionVersion =
		dtoh16a(&data[PTP_di_VendorExtensionVersion]);
	di->VendorExtensionDesc = 
		ptp_unpack_string(params, data,
		PTP_di_VendorExtensionDesc, &len); 
	totallen=len*2+1;
	di->FunctionalMode = 
		dtoh16a(&data[PTP_di_FunctionalMode+totallen]);
	di->OperationsSupported_len = ptp_unpack_uint16_t_array(params, data,
		PTP_di_OperationsSupported+totallen,
		&di->OperationsSupported);
	totallen=totallen+di->OperationsSupported_len*sizeof(uint16_t)+sizeof(uint32_t);
	di->EventsSupported_len = ptp_unpack_uint16_t_array(params, data,
		PTP_di_OperationsSupported+totallen,
		&di->EventsSupported);
	totallen=totallen+di->EventsSupported_len*sizeof(uint16_t)+sizeof(uint32_t);
	di->DevicePropertiesSupported_len =
		ptp_unpack_uint16_t_array(params, data,
		PTP_di_OperationsSupported+totallen,
		&di->DevicePropertiesSupported);
	totallen=totallen+di->DevicePropertiesSupported_len*sizeof(uint16_t)+sizeof(uint32_t);
	di->CaptureFormats_len = ptp_unpack_uint16_t_array(params, data,
		PTP_di_OperationsSupported+totallen,
		&di->CaptureFormats);
	totallen=totallen+di->CaptureFormats_len*sizeof(uint16_t)+sizeof(uint32_t);
	di->ImageFormats_len = ptp_unpack_uint16_t_array(params, data,
		PTP_di_OperationsSupported+totallen,
		&di->ImageFormats);
	totallen=totallen+di->ImageFormats_len*sizeof(uint16_t)+sizeof(uint32_t);
	di->Manufacturer = ptp_unpack_string(params, data,
		PTP_di_OperationsSupported+totallen,
		&len);
	totallen+=len*2+1;
	di->Model = ptp_unpack_string(params, data,
		PTP_di_OperationsSupported+totallen,
		&len);
	totallen+=len*2+1;
	di->DeviceVersion = ptp_unpack_string(params, data,
		PTP_di_OperationsSupported+totallen,
		&len);
	totallen+=len*2+1;
	di->SerialNumber = ptp_unpack_string(params, data,
		PTP_di_OperationsSupported+totallen,
		&len);
}
	
/* ObjectHandles array pack/unpack */

#define PTP_oh				 0

static inline void
ptp_unpack_OH (PTPParams *params, unsigned char* data, PTPObjectHandles *oh, unsigned int len)
{
	oh->n = ptp_unpack_uint32_t_array(params, data, PTP_oh, &oh->Handler);
}

/* StoreIDs array pack/unpack */

#define PTP_sids			 0

static inline void
ptp_unpack_SIDs (PTPParams *params, unsigned char* data, PTPStorageIDs *sids, unsigned int len)
{
	sids->n = ptp_unpack_uint32_t_array(params, data, PTP_sids,
	&sids->Storage);
}

/* StorageInfo pack/unpack */

#define PTP_si_StorageType		 0
#define PTP_si_FilesystemType		 2
#define PTP_si_AccessCapability		 4
#define PTP_si_MaxCapability		 6
#define PTP_si_FreeSpaceInBytes		14
#define PTP_si_FreeSpaceInImages	22
#define PTP_si_StorageDescription	26

static inline void
ptp_unpack_SI (PTPParams *params, unsigned char* data, PTPStorageInfo *si, unsigned int len)
{
	uint8_t storagedescriptionlen;

	si->StorageType=dtoh16a(&data[PTP_si_StorageType]);
	si->FilesystemType=dtoh16a(&data[PTP_si_FilesystemType]);
	si->AccessCapability=dtoh16a(&data[PTP_si_AccessCapability]);
	si->MaxCapability=dtoh64a(&data[PTP_si_MaxCapability]);
	si->FreeSpaceInBytes=dtoh64a(&data[PTP_si_FreeSpaceInBytes]);
	si->FreeSpaceInImages=dtoh32a(&data[PTP_si_FreeSpaceInImages]);
	si->StorageDescription=ptp_unpack_string(params, data,
		PTP_si_StorageDescription, &storagedescriptionlen);
	si->VolumeLabel=ptp_unpack_string(params, data,
		PTP_si_StorageDescription+storagedescriptionlen*2+1,
		&storagedescriptionlen);
}

/* ObjectInfo pack/unpack */

#define PTP_oi_StorageID		 0
#define PTP_oi_ObjectFormat		 4
#define PTP_oi_ProtectionStatus		 6
#define PTP_oi_ObjectCompressedSize	 8
#define PTP_oi_ThumbFormat		12
#define PTP_oi_ThumbCompressedSize	14
#define PTP_oi_ThumbPixWidth		18
#define PTP_oi_ThumbPixHeight		22
#define PTP_oi_ImagePixWidth		26
#define PTP_oi_ImagePixHeight		30
#define PTP_oi_ImageBitDepth		34
#define PTP_oi_ParentObject		38
#define PTP_oi_AssociationType		42
#define PTP_oi_AssociationDesc		44
#define PTP_oi_SequenceNumber		48
#define PTP_oi_filenamelen		52
#define PTP_oi_Filename			53

static inline uint32_t
ptp_pack_OI (PTPParams *params, PTPObjectInfo *oi, unsigned char** oidataptr)
{
	unsigned char* oidata;
	uint8_t filenamelen;
	uint8_t capturedatelen=0;
	/* let's allocate some memory first; XXX i'm sure it's wrong */
	oidata=malloc(PTP_oi_Filename+(strlen(oi->Filename)+1)*2+4);
	/* the caller should free it after use! */
#if 0
	char *capture_date="20020101T010101"; /* XXX Fake date */
#endif
	memset (oidata, 0, (PTP_oi_Filename+(strlen(oi->Filename)+1)*2+4));
	htod32a(&oidata[PTP_oi_StorageID],oi->StorageID);
	htod16a(&oidata[PTP_oi_ObjectFormat],oi->ObjectFormat);
	htod16a(&oidata[PTP_oi_ProtectionStatus],oi->ProtectionStatus);
	htod32a(&oidata[PTP_oi_ObjectCompressedSize],oi->ObjectCompressedSize);
	htod16a(&oidata[PTP_oi_ThumbFormat],oi->ThumbFormat);
	htod32a(&oidata[PTP_oi_ThumbCompressedSize],oi->ThumbCompressedSize);
	htod32a(&oidata[PTP_oi_ThumbPixWidth],oi->ThumbPixWidth);
	htod32a(&oidata[PTP_oi_ThumbPixHeight],oi->ThumbPixHeight);
	htod32a(&oidata[PTP_oi_ImagePixWidth],oi->ImagePixWidth);
	htod32a(&oidata[PTP_oi_ImagePixHeight],oi->ImagePixHeight);
	htod32a(&oidata[PTP_oi_ImageBitDepth],oi->ImageBitDepth);
	htod32a(&oidata[PTP_oi_ParentObject],oi->ParentObject);
	htod16a(&oidata[PTP_oi_AssociationType],oi->AssociationType);
	htod32a(&oidata[PTP_oi_AssociationDesc],oi->AssociationDesc);
	htod32a(&oidata[PTP_oi_SequenceNumber],oi->SequenceNumber);
	
	ptp_pack_string(params, oi->Filename, oidata, PTP_oi_filenamelen, &filenamelen);
/*
	filenamelen=(uint8_t)strlen(oi->Filename);
	htod8a(&req->data[PTP_oi_filenamelen],filenamelen+1);
	for (i=0;i<filenamelen && i< PTP_MAXSTRLEN; i++) {
		req->data[PTP_oi_Filename+i*2]=oi->Filename[i];
	}
*/
	/*
	 *XXX Fake date.
	 * for example Kodak sets Capture date on the basis of EXIF data.
	 * Spec says that this field is from perspective of Initiator.
	 */
#if 0	/* seems now we don't need any data packed in OI dataset... for now ;)*/
	capturedatelen=strlen(capture_date);
	htod8a(&data[PTP_oi_Filename+(filenamelen+1)*2],
		capturedatelen+1);
	for (i=0;i<capturedatelen && i< PTP_MAXSTRLEN; i++) {
		data[PTP_oi_Filename+(i+filenamelen+1)*2+1]=capture_date[i];
	}
	htod8a(&data[PTP_oi_Filename+(filenamelen+capturedatelen+2)*2+1],
		capturedatelen+1);
	for (i=0;i<capturedatelen && i< PTP_MAXSTRLEN; i++) {
		data[PTP_oi_Filename+(i+filenamelen+capturedatelen+2)*2+2]=
		  capture_date[i];
	}
#endif
	/* XXX this function should return dataset length */
	
	*oidataptr=oidata;
	return (PTP_oi_Filename+(filenamelen+1)*2+(capturedatelen+1)*4);
}

static inline void
ptp_unpack_OI (PTPParams *params, unsigned char* data, PTPObjectInfo *oi, unsigned int len)
{
	uint8_t filenamelen;
	uint8_t capturedatelen;
	char *capture_date;
	char tmp[16];
	struct tm tm;

	memset(&tm,0,sizeof(tm));

	oi->StorageID=dtoh32a(&data[PTP_oi_StorageID]);
	oi->ObjectFormat=dtoh16a(&data[PTP_oi_ObjectFormat]);
	oi->ProtectionStatus=dtoh16a(&data[PTP_oi_ProtectionStatus]);
	oi->ObjectCompressedSize=dtoh32a(&data[PTP_oi_ObjectCompressedSize]);
	oi->ThumbFormat=dtoh16a(&data[PTP_oi_ThumbFormat]);
	oi->ThumbCompressedSize=dtoh32a(&data[PTP_oi_ThumbCompressedSize]);
	oi->ThumbPixWidth=dtoh32a(&data[PTP_oi_ThumbPixWidth]);
	oi->ThumbPixHeight=dtoh32a(&data[PTP_oi_ThumbPixHeight]);
	oi->ImagePixWidth=dtoh32a(&data[PTP_oi_ImagePixWidth]);
	oi->ImagePixHeight=dtoh32a(&data[PTP_oi_ImagePixHeight]);
	oi->ImageBitDepth=dtoh32a(&data[PTP_oi_ImageBitDepth]);
	oi->ParentObject=dtoh32a(&data[PTP_oi_ParentObject]);
	oi->AssociationType=dtoh16a(&data[PTP_oi_AssociationType]);
	oi->AssociationDesc=dtoh32a(&data[PTP_oi_AssociationDesc]);
	oi->SequenceNumber=dtoh32a(&data[PTP_oi_SequenceNumber]);
	oi->Filename= ptp_unpack_string(params, data, PTP_oi_filenamelen, &filenamelen);

	capture_date = ptp_unpack_string(params, data,
		PTP_oi_filenamelen+filenamelen*2+1, &capturedatelen);
	/* subset of ISO 8601, without '.s' tenths of second and 
	 * time zone
	 */
	if (capturedatelen>15)
	{
		strncpy (tmp, capture_date, 4);
		tmp[4] = 0;
		tm.tm_year=atoi (tmp) - 1900;
		strncpy (tmp, capture_date + 4, 2);
		tmp[2] = 0;
		tm.tm_mon = atoi (tmp) - 1;
		strncpy (tmp, capture_date + 6, 2);
		tmp[2] = 0;
		tm.tm_mday = atoi (tmp);
		strncpy (tmp, capture_date + 9, 2);
		tmp[2] = 0;
		tm.tm_hour = atoi (tmp);
		strncpy (tmp, capture_date + 11, 2);
		tmp[2] = 0;
		tm.tm_min = atoi (tmp);
		strncpy (tmp, capture_date + 13, 2);
		tmp[2] = 0;
		tm.tm_sec = atoi (tmp);
		oi->CaptureDate=mktime (&tm);
	}
	free(capture_date);

	/* now it's modification date ;) */
	capture_date = ptp_unpack_string(params, data,
		PTP_oi_filenamelen+filenamelen*2
		+capturedatelen*2+2,&capturedatelen);
	if (capturedatelen>15)
	{
		strncpy (tmp, capture_date, 4);
		tmp[4] = 0;
		tm.tm_year=atoi (tmp) - 1900;
		strncpy (tmp, capture_date + 4, 2);
		tmp[2] = 0;
		tm.tm_mon = atoi (tmp) - 1;
		strncpy (tmp, capture_date + 6, 2);
		tmp[2] = 0;
		tm.tm_mday = atoi (tmp);
		strncpy (tmp, capture_date + 9, 2);
		tmp[2] = 0;
		tm.tm_hour = atoi (tmp);
		strncpy (tmp, capture_date + 11, 2);
		tmp[2] = 0;
		tm.tm_min = atoi (tmp);
		strncpy (tmp, capture_date + 13, 2);
		tmp[2] = 0;
		tm.tm_sec = atoi (tmp);
		oi->ModificationDate=mktime (&tm);
	}
	free(capture_date);
}

/* Custom Type Value Assignement (without Length) macro frequently used below */
#define CTVAL(target,func) {			\
	if (total - *offset < sizeof(target))	\
		return 0;			\
	target = func(&data[*offset]);		\
	*offset += sizeof(target);		\
}

#define RARR(val,member,func)	{			\
	int n,j;					\
	if (total - *offset < sizeof(uint32_t))		\
		return 0;				\
	n = dtoh32a (&data[*offset]);			\
	*offset += sizeof(uint32_t);			\
							\
	val->a.count = n;				\
	val->a.v = malloc(sizeof(val->a.v[0])*n);	\
	if (!val->a.v) return 0;			\
	for (j=0;j<n;j++)				\
		CTVAL(val->a.v[j].member, func);	\
}

static inline int
ptp_unpack_DPV (
	PTPParams *params, unsigned char* data, int *offset, int total,
	PTPPropertyValue* value, uint16_t datatype
) {
	switch (datatype) {
	case PTP_DTC_INT8:
		CTVAL(value->i8,dtoh8a);
		break;
	case PTP_DTC_UINT8:
		CTVAL(value->u8,dtoh8a);
		break;
	case PTP_DTC_INT16:
		CTVAL(value->i16,dtoh16a);
		break;
	case PTP_DTC_UINT16:
		CTVAL(value->u16,dtoh16a);
		break;
	case PTP_DTC_INT32:
		CTVAL(value->i32,dtoh32a);
		break;
	case PTP_DTC_UINT32:
		CTVAL(value->u32,dtoh32a);
		break;
	case PTP_DTC_AINT8:
		RARR(value,i8,dtoh8a);
		break;
	case PTP_DTC_AUINT8:
		RARR(value,u8,dtoh8a);
		break;
	case PTP_DTC_AUINT16:
		RARR(value,u16,dtoh16a);
		break;
	case PTP_DTC_AINT16:
		RARR(value,i16,dtoh16a);
		break;
	case PTP_DTC_AUINT32:
		RARR(value,u32,dtoh32a);
		break;
	case PTP_DTC_AINT32:
		RARR(value,i32,dtoh32a);
		break;
	/* XXX: other int types are unimplemented */
	/* XXX: other int arrays are unimplemented also */
	case PTP_DTC_STR: {
		uint8_t len;
		/* XXX: max size */
		value->str = ptp_unpack_string(params,data,*offset,&len);
		*offset += len*2+1;
		if (!value->str)
			return 0;
		break;
	}
	}
	return 1;
}

/* Device Property pack/unpack */

#define PTP_dpd_DevicePropertyCode	0
#define PTP_dpd_DataType		2
#define PTP_dpd_GetSet			4
#define PTP_dpd_FactoryDefaultValue	5

static inline int
ptp_unpack_DPD (PTPParams *params, unsigned char* data, PTPDevicePropDesc *dpd, unsigned int dpdlen)
{
	int offset=0, ret;

	memset (dpd, 0, sizeof(*dpd));
	dpd->DevicePropertyCode=dtoh16a(&data[PTP_dpd_DevicePropertyCode]);
	dpd->DataType=dtoh16a(&data[PTP_dpd_DataType]);
	dpd->GetSet=dtoh8a(&data[PTP_dpd_GetSet]);

	offset = PTP_dpd_FactoryDefaultValue;
	ret = ptp_unpack_DPV (params, data, &offset, dpdlen, &dpd->FactoryDefaultValue, dpd->DataType);
	if (!ret) goto outofmemory;
	ret = ptp_unpack_DPV (params, data, &offset, dpdlen, &dpd->CurrentValue, dpd->DataType);
	if (!ret) goto outofmemory;

	/* if offset==0 then Data Type format is not supported by this
	   code or the Data Type is a string (with two empty strings as
	   values). In both cases Form Flag should be set to 0x00 and FORM is
	   not present. */

	dpd->FormFlag=PTP_DPFF_None;
	if (offset==PTP_dpd_FactoryDefaultValue)
		return 1;

	dpd->FormFlag=dtoh8a(&data[offset]);
	offset+=sizeof(uint8_t);

	switch (dpd->FormFlag) {
	case PTP_DPFF_Range:
		ret = ptp_unpack_DPV (params, data, &offset, dpdlen, &dpd->FORM.Range.MinimumValue, dpd->DataType);
		if (!ret) goto outofmemory;
		ret = ptp_unpack_DPV (params, data, &offset, dpdlen, &dpd->FORM.Range.MaximumValue, dpd->DataType);
		if (!ret) goto outofmemory;
		ret = ptp_unpack_DPV (params, data, &offset, dpdlen, &dpd->FORM.Range.StepSize, dpd->DataType);
		if (!ret) goto outofmemory;
		break;
	case PTP_DPFF_Enumeration: {
		int i;
#define N	dpd->FORM.Enum.NumberOfValues
		N = dtoh16a(&data[offset]);
		offset+=sizeof(uint16_t);
		dpd->FORM.Enum.SupportedValue = malloc(N*sizeof(dpd->FORM.Enum.SupportedValue[0]));
		if (!dpd->FORM.Enum.SupportedValue)
			goto outofmemory;

		memset (dpd->FORM.Enum.SupportedValue,0 , N*sizeof(dpd->FORM.Enum.SupportedValue[0]));
		for (i=0;i<N;i++) {
			ret = ptp_unpack_DPV (params, data, &offset, dpdlen, &dpd->FORM.Enum.SupportedValue[i], dpd->DataType);

			/* Slightly different handling here. The HP PhotoSmart 120
			 * specifies an enumeration with N in wrong endian
			 * 00 01 instead of 01 00, so we count the enum just until the
			 * the end of the packet.
			 */
			if (!ret) {
				if (!i)
					goto outofmemory;
				dpd->FORM.Enum.NumberOfValues = i;
				break;
			}
		}
		}
	}
#undef N
	return 1;
outofmemory:
	ptp_free_devicepropdesc(dpd);
	return 0;
}

/* (MTP) Object Property pack/unpack */
#define PTP_opd_ObjectPropertyCode	0
#define PTP_opd_DataType		2
#define PTP_opd_GetSet			4
#define PTP_opd_FactoryDefaultValue	5

static inline int
ptp_unpack_OPD (PTPParams *params, unsigned char* data, PTPObjectPropDesc *opd, unsigned int opdlen)
{
	int offset=0, ret;

	memset (opd, 0, sizeof(*opd));
	opd->ObjectPropertyCode=dtoh16a(&data[PTP_opd_ObjectPropertyCode]);
	opd->DataType=dtoh16a(&data[PTP_opd_DataType]);
	opd->GetSet=dtoh8a(&data[PTP_opd_GetSet]);

	offset = PTP_opd_FactoryDefaultValue;
	ret = ptp_unpack_DPV (params, data, &offset, opdlen, &opd->FactoryDefaultValue, opd->DataType);
	if (!ret) goto outofmemory;

	opd->GroupCode=dtoh32a(&data[offset]);
	offset+=sizeof(uint32_t);

	opd->FormFlag=dtoh8a(&data[offset]);
	offset+=sizeof(uint8_t);

	switch (opd->FormFlag) {
	case PTP_OPFF_Range:
		ret = ptp_unpack_DPV (params, data, &offset, opdlen, &opd->FORM.Range.MinimumValue, opd->DataType);
		if (!ret) goto outofmemory;
		ret = ptp_unpack_DPV (params, data, &offset, opdlen, &opd->FORM.Range.MaximumValue, opd->DataType);
		if (!ret) goto outofmemory;
		ret = ptp_unpack_DPV (params, data, &offset, opdlen, &opd->FORM.Range.StepSize, opd->DataType);
		if (!ret) goto outofmemory;
		break;
	case PTP_OPFF_Enumeration: {
		int i;
#define N	opd->FORM.Enum.NumberOfValues
		N = dtoh16a(&data[offset]);
		offset+=sizeof(uint16_t);
		opd->FORM.Enum.SupportedValue = malloc(N*sizeof(opd->FORM.Enum.SupportedValue[0]));
		if (!opd->FORM.Enum.SupportedValue)
			goto outofmemory;

		memset (opd->FORM.Enum.SupportedValue,0 , N*sizeof(opd->FORM.Enum.SupportedValue[0]));
		for (i=0;i<N;i++) {
			ret = ptp_unpack_DPV (params, data, &offset, opdlen, &opd->FORM.Enum.SupportedValue[i], opd->DataType);

			/* Slightly different handling here. The HP PhotoSmart 120
			 * specifies an enumeration with N in wrong endian
			 * 00 01 instead of 01 00, so we count the enum just until the
			 * the end of the packet.
			 */
			if (!ret) {
				if (!i)
					goto outofmemory;
				opd->FORM.Enum.NumberOfValues = i;
				break;
			}
		}
#undef N
		}
	}
	return 1;
outofmemory:
	ptp_free_objectpropdesc(opd);
	return 0;
}


static inline uint32_t
ptp_pack_DPV (PTPParams *params, PTPPropertyValue* value, unsigned char** dpvptr, uint16_t datatype)
{
	unsigned char* dpv=NULL;
	uint32_t size=0;
	int	i;

	switch (datatype) {
	case PTP_DTC_INT8:
		size=sizeof(int8_t);
		dpv=malloc(size);
		htod8a(dpv,value->i8);
		break;
	case PTP_DTC_UINT8:
		size=sizeof(uint8_t);
		dpv=malloc(size);
		htod8a(dpv,value->u8);
		break;
	case PTP_DTC_INT16:
		size=sizeof(int16_t);
		dpv=malloc(size);
		htod16a(dpv,value->i16);
		break;
	case PTP_DTC_UINT16:
		size=sizeof(uint16_t);
		dpv=malloc(size);
		htod16a(dpv,value->u16);
		break;
	case PTP_DTC_INT32:
		size=sizeof(int32_t);
		dpv=malloc(size);
		htod32a(dpv,value->i32);
		break;
	case PTP_DTC_UINT32:
		size=sizeof(uint32_t);
		dpv=malloc(size);
		htod32a(dpv,value->u32);
		break;
	case PTP_DTC_AUINT8:
		size=sizeof(uint32_t)+value->a.count*sizeof(uint8_t);
		dpv=malloc(size);
		htod32a(dpv,value->a.count);
		for (i=0;i<value->a.count;i++)
			htod8a(&dpv[4+i],value->a.v[i].u8);
		break;
	case PTP_DTC_AINT8:
		size=sizeof(uint32_t)+value->a.count*sizeof(int8_t);
		dpv=malloc(size);
		htod32a(dpv,value->a.count);
		for (i=0;i<value->a.count;i++)
			htod8a(&dpv[4+i],value->a.v[i].i8);
		break;
	case PTP_DTC_AUINT16:
		size=sizeof(uint32_t)+value->a.count*sizeof(uint16_t);
		dpv=malloc(size);
		htod32a(dpv,value->a.count);
		for (i=0;i<value->a.count;i++)
			htod16a(&dpv[4+i],value->a.v[i].u16);
		break;
	case PTP_DTC_AINT16:
		size=sizeof(uint32_t)+value->a.count*sizeof(int16_t);
		dpv=malloc(size);
		htod32a(dpv,value->a.count);
		for (i=0;i<value->a.count;i++)
			htod16a(&dpv[4+i],value->a.v[i].i16);
		break;
	case PTP_DTC_AUINT32:
		size=sizeof(uint32_t)+value->a.count*sizeof(uint32_t);
		dpv=malloc(size);
		htod32a(dpv,value->a.count);
		for (i=0;i<value->a.count;i++)
			htod32a(&dpv[4+i],value->a.v[i].u32);
		break;
	case PTP_DTC_AINT32:
		size=sizeof(uint32_t)+value->a.count*sizeof(int32_t);
		dpv=malloc(size);
		htod32a(dpv,value->a.count);
		for (i=0;i<value->a.count;i++)
			htod32a(&dpv[4+i],value->a.v[i].i32);
		break;
	/* XXX: other int types are unimplemented */
	case PTP_DTC_STR: {
		dpv=ptp_get_packed_stringcopy(params, value->str, &size);
		break;
	}
	}
	*dpvptr=dpv;
	return size;
}

#define MAX_MTP_PROPS 127
static inline uint32_t
ptp_pack_OPL (PTPParams *params, MTPPropList *proplist, unsigned char** opldataptr, uint32_t objectid)
{
	unsigned char* opldata;
	MTPPropList *propitr;
	unsigned char *packedprops[MAX_MTP_PROPS];
	uint32_t packedpropslens[MAX_MTP_PROPS];
	uint16_t packedpropsids[MAX_MTP_PROPS];
	uint16_t packedpropstypes[MAX_MTP_PROPS];
	uint32_t totalsize = 0;
	uint32_t bufp = 0;
	uint32_t noitems = 0;
	uint32_t i;

	totalsize = sizeof(uint32_t); /* 4 bytes to store the number of elements */
	propitr = proplist;
	while (propitr != NULL && noitems < MAX_MTP_PROPS) {
		/* Overhead for each item */
		totalsize += sizeof(uint32_t); /* Object ID */
		/* Metadata type */
		packedpropsids[noitems]=propitr->property;
		totalsize += sizeof(uint16_t);
		/* Data type */
		packedpropstypes[noitems]= propitr->datatype;
		totalsize += sizeof(uint16_t);
		/* Add each property to be sent. */
	        packedpropslens[noitems] = ptp_pack_DPV (params, &propitr->propval, &packedprops[noitems], propitr->datatype);
		totalsize += packedpropslens[noitems];
		noitems ++;
		propitr = propitr->next;
	}

	/* Allocate memory for the packed property list */
	opldata = malloc(totalsize);

	htod32a(&opldata[bufp],noitems);
	bufp += 4;

	/* Copy into a nice packed list */
	for (i = 0; i < noitems; i++) {
		/* Object ID */
		htod32a(&opldata[bufp],objectid);
		bufp += sizeof(uint32_t);
		htod16a(&opldata[bufp],packedpropsids[i]);
		bufp += sizeof(uint16_t);
		htod16a(&opldata[bufp],packedpropstypes[i]);
		bufp += sizeof(uint16_t);
		/* The copy the actual property */
		memcpy(&opldata[bufp], packedprops[i], packedpropslens[i]);
		bufp += packedpropslens[i];
		free(packedprops[i]);
	}
	*opldataptr = opldata;
	return totalsize;
}

static inline int
ptp_unpack_OPL (PTPParams *params, unsigned char* data, MTPPropList **proplist, unsigned int len)
{ 
	uint32_t prop_count = dtoh32a(data);
	MTPPropList *prop = NULL;
	int offset = 0, i;

	if (prop_count == 0) {
		*proplist = NULL;
		return 0;
	}
	data += sizeof(uint32_t);
	*proplist = malloc(sizeof(MTPPropList));
	prop = *proplist;
	for (i = 0; i < prop_count; i++) {
		/* we ignore the object handle */
		data += sizeof(uint32_t);
		len -= sizeof(uint32_t);

		prop->property = dtoh32a(data);
		data += sizeof(uint16_t);
		len -= sizeof(uint16_t);

		prop->datatype = dtoh32a(data);
		data += sizeof(uint16_t);
		len -= sizeof(uint16_t);

		offset = 0;
		ptp_unpack_DPV(params, data, &offset, len, &prop->propval, prop->datatype);
		data += offset;
		len -= offset;

		if (i != prop_count - 1) {
			prop->next = malloc(sizeof(MTPPropList));
			prop = prop->next;
		} else
			prop->next = NULL;
	}
	return prop_count;
}

/*
    PTP USB Event container unpack
    Copyright (c) 2003 Nikolai Kopanygin
*/

#define PTP_ec_Length		0
#define PTP_ec_Type		4
#define PTP_ec_Code		6
#define PTP_ec_TransId		8
#define PTP_ec_Param1		12
#define PTP_ec_Param2		16
#define PTP_ec_Param3		20

static inline void
ptp_unpack_EC (PTPParams *params, unsigned char* data, PTPUSBEventContainer *ec, unsigned int len)
{
	if (data==NULL)
		return;
	ec->length=dtoh32a(&data[PTP_ec_Length]);
	ec->type=dtoh16a(&data[PTP_ec_Type]);
	ec->code=dtoh16a(&data[PTP_ec_Code]);
	ec->trans_id=dtoh32a(&data[PTP_ec_TransId]);

	if (ec->length>=(PTP_ec_Param1+4))
		ec->param1=dtoh32a(&data[PTP_ec_Param1]);
	else
		ec->param1=0;
	if (ec->length>=(PTP_ec_Param2+4))
		ec->param2=dtoh32a(&data[PTP_ec_Param2]);
	else
		ec->param2=0;
	if (ec->length>=(PTP_ec_Param3+4))
		ec->param3=dtoh32a(&data[PTP_ec_Param3]);
	else
		ec->param3=0;
}

/*
    PTP Canon Folder Entry unpack
    Copyright (c) 2003 Nikolai Kopanygin
*/
#define PTP_cfe_ObjectHandle		0
#define PTP_cfe_ObjectFormatCode	4
#define PTP_cfe_Flags			6
#define PTP_cfe_ObjectSize		7
#define PTP_cfe_Time			11
#define PTP_cfe_Filename		15

static inline void
ptp_unpack_Canon_FE (PTPParams *params, unsigned char* data, PTPCANONFolderEntry *fe)
{
	int i;
	if (data==NULL)
		return;
	fe->ObjectHandle=dtoh32a(&data[PTP_cfe_ObjectHandle]);
	fe->ObjectFormatCode=dtoh16a(&data[PTP_cfe_ObjectFormatCode]);
	fe->Flags=dtoh8a(&data[PTP_cfe_Flags]);
	fe->ObjectSize=dtoh32a((unsigned char*)&data[PTP_cfe_ObjectSize]);
	fe->Time=(time_t)dtoh32a(&data[PTP_cfe_Time]);
	for (i=0; i<PTP_CANON_FilenameBufferLen; i++)
		fe->Filename[i]=(char)dtoh8a(&data[PTP_cfe_Filename+i]);
}

/*
    PTP USB Event container unpack for Nikon events.
*/
#define PTP_nikon_ec_Length		0
#define PTP_nikon_ec_Code		2
#define PTP_nikon_ec_Param1		4
#define PTP_nikon_ec_Size		6
static inline void
ptp_unpack_Nikon_EC (PTPParams *params, unsigned char* data, unsigned int len, PTPUSBEventContainer **ec, int *cnt)
{
	int i;

	*ec = NULL;
	if (data == NULL)
		return;
	if (len < PTP_nikon_ec_Code)
		return;
	*cnt = dtoh16a(&data[PTP_nikon_ec_Length]);
	if (*cnt > (len-PTP_nikon_ec_Code)/PTP_nikon_ec_Size) /* broken cnt? */
		return;
	*ec = malloc(sizeof(PTPUSBEventContainer)*(*cnt));
	
	for (i=0;i<*cnt;i++) {
		memset(&(*ec)[i],0,sizeof(PTPUSBEventContainer));
		(*ec)[i].code	= dtoh16a(&data[PTP_nikon_ec_Code+PTP_nikon_ec_Size*i]);
		(*ec)[i].param1	= dtoh32a(&data[PTP_nikon_ec_Param1+PTP_nikon_ec_Size*i]);
	}
}


static inline uint32_t
ptp_pack_EK_text(PTPParams *params, PTPEKTextParams *text, unsigned char **data) {
	int i, len = 0;
	uint8_t	retlen;
	unsigned char *curdata;

	len =	2*(strlen(text->title)+1)+1+
		2*(strlen(text->line[0])+1)+1+
		2*(strlen(text->line[1])+1)+1+
		2*(strlen(text->line[2])+1)+1+
		2*(strlen(text->line[3])+1)+1+
		2*(strlen(text->line[4])+1)+1+
		4*2+2*4+2+4+2+5*4*2;
	*data = malloc(len);
	if (!*data) return 0;

	curdata = *data;
	htod16a(curdata,100);curdata+=2;
	htod16a(curdata,1);curdata+=2;
	htod16a(curdata,0);curdata+=2;
	htod16a(curdata,1000);curdata+=2;

	htod32a(curdata,0);curdata+=4;
	htod32a(curdata,0);curdata+=4;

	htod16a(curdata,6);curdata+=2;
	htod32a(curdata,0);curdata+=4;

	ptp_pack_string(params, text->title, curdata, 0, &retlen); curdata+=2*retlen+1;htod16a(curdata,0);curdata+=2;
	htod16a(curdata,0x10);curdata+=2;
	
	for (i=0;i<5;i++) {
		ptp_pack_string(params, text->line[i], curdata, 0, &retlen); curdata+=2*retlen+1;htod16a(curdata,0);curdata+=2;
		htod16a(curdata,0x10);curdata+=2;
		htod16a(curdata,0x01);curdata+=2;
		htod16a(curdata,0x02);curdata+=2;
		htod16a(curdata,0x06);curdata+=2;
	}
	return len;
}

#define ptp_canon_dir_version	0x00
#define ptp_canon_dir_ofc	0x02
#define ptp_canon_dir_unk1	0x04
#define ptp_canon_dir_objectid	0x08
#define ptp_canon_dir_parentid	0x0c
#define ptp_canon_dir_previd	0x10	/* in same dir */
#define ptp_canon_dir_nextid	0x14	/* in same dir */
#define ptp_canon_dir_nextchild	0x18	/* down one dir */
#define ptp_canon_dir_storageid	0x1c	/* only in storage entry */
#define ptp_canon_dir_name	0x20
#define ptp_canon_dir_flags	0x2c
#define ptp_canon_dir_size	0x30
#define ptp_canon_dir_unixtime	0x34
#define ptp_canon_dir_year	0x38
#define ptp_canon_dir_month	0x39
#define ptp_canon_dir_mday	0x3a
#define ptp_canon_dir_hour	0x3b
#define ptp_canon_dir_minute	0x3c
#define ptp_canon_dir_second	0x3d
#define ptp_canon_dir_unk2	0x3e
#define ptp_canon_dir_thumbsize	0x40
#define ptp_canon_dir_width	0x44
#define ptp_canon_dir_height	0x48

static inline uint16_t
ptp_unpack_canon_directory (
	PTPParams		*params,
	unsigned char		*dir,
	uint32_t		cnt,
	PTPObjectHandles	*handles,
	PTPObjectInfo		**oinfos,	/* size(handles->n) */
	uint32_t		**flags		/* size(handles->n) */
) {
	unsigned int	i, j, nrofobs = 0, curob = 0;

#define ISOBJECT(ptr) (dtoh32a((ptr)+ptp_canon_dir_storageid) == 0xffffffff)
	for (i=0;i<cnt;i++)
		if (ISOBJECT(dir+i*0x4c)) nrofobs++;
	handles->n = nrofobs;
	handles->Handler = calloc(sizeof(handles->Handler[0]),nrofobs);
	if (!handles->Handler) return PTP_RC_GeneralError;
	*oinfos = calloc(sizeof((*oinfos)[0]),nrofobs);
	if (!*oinfos) return PTP_RC_GeneralError;
	*flags  = calloc(sizeof((*flags)[0]),nrofobs);
	if (!*flags) return PTP_RC_GeneralError;

	/* Migrate data into objects ids, handles into
	 * the object handler array.
	 */
	curob = 0;
	for (i=0;i<cnt;i++) {
		unsigned char	*cur = dir+i*0x4c;
		PTPObjectInfo	*oi = (*oinfos)+curob;

		if (!ISOBJECT(cur))
			continue;

		handles->Handler[curob] = dtoh32a(cur + ptp_canon_dir_objectid);
		oi->StorageID		= 0xffffffff;
		oi->ObjectFormat	= dtoh16a(cur + ptp_canon_dir_ofc);
		oi->ParentObject	= dtoh32a(cur + ptp_canon_dir_parentid);
		oi->Filename		= strdup((char*)(cur + ptp_canon_dir_name));
		oi->ObjectCompressedSize= dtoh32a(cur + ptp_canon_dir_size);
		oi->ThumbCompressedSize	= dtoh32a(cur + ptp_canon_dir_thumbsize);
		oi->ImagePixWidth	= dtoh32a(cur + ptp_canon_dir_width);
		oi->ImagePixHeight	= dtoh32a(cur + ptp_canon_dir_height);
		oi->CaptureDate		= oi->ModificationDate = dtoh32a(cur + ptp_canon_dir_unixtime);
		(*flags)[curob]		= dtoh32a(cur + ptp_canon_dir_flags);
		curob++;
	}
	/* Walk over Storage ID entries and distribute the IDs to
	 * the parent objects. */
	for (i=0;i<cnt;i++) {
		unsigned char	*cur = dir+i*0x4c;
		uint32_t	nextchild = dtoh32a(cur + ptp_canon_dir_nextchild);

		if (ISOBJECT(cur))
			continue;
		for (j=0;j<handles->n;j++) if (nextchild == handles->Handler[j]) break;
		if (j == handles->n) continue;
		(*oinfos)[j].StorageID = dtoh32a(cur + ptp_canon_dir_storageid);
	}
	/* Walk over all objects and distribute the storage ids */
	while (1) {
		int changed = 0;
		for (i=0;i<cnt;i++) {
			unsigned char	*cur = dir+i*0x4c;
			uint32_t	oid = dtoh32a(cur + ptp_canon_dir_objectid);
			uint32_t	nextoid = dtoh32a(cur + ptp_canon_dir_nextid);
			uint32_t	nextchild = dtoh32a(cur + ptp_canon_dir_nextchild);
			uint32_t	storageid;

			if (!ISOBJECT(cur))
				continue;
			for (j=0;j<handles->n;j++) if (oid == handles->Handler[j]) break;
			if (j == handles->n) {
				/*fprintf(stderr,"did not find oid in lookup pass for current oid\n");*/
				continue;
			}
	 		storageid = (*oinfos)[j].StorageID;
			if (storageid == 0xffffffff) continue;
			if (nextoid != 0xffffffff) {
				for (j=0;j<handles->n;j++) if (nextoid == handles->Handler[j]) break;
				if (j == handles->n) {
					/*fprintf(stderr,"did not find oid in lookup pass for next oid\n");*/
					continue;
				}
				if ((*oinfos)[j].StorageID == 0xffffffff) {
					(*oinfos)[j].StorageID = storageid;
					changed++;
				}
			}
			if (nextchild != 0xffffffff) {
				for (j=0;j<handles->n;j++) if (nextchild == handles->Handler[j]) break;
				if (j == handles->n) {
					/*fprintf(stderr,"did not find oid in lookup pass for next child\n");*/
					continue;
				}
				if ((*oinfos)[j].StorageID == 0xffffffff) {
					(*oinfos)[j].StorageID = storageid;
					changed++;
				}
			}
		}
		/* Check if we:
		 * - changed no entry (nothing more to do)
		 * - changed all of them at once (usually happens)
		 * break if we do.
		 */
		if (!changed || (changed==nrofobs-1))
			break;
	}
#undef ISOBJECT
	return PTP_RC_OK;
}

