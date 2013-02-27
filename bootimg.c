/* tools/mkbootimg/mkbootimg.c
**
** Copyright 2007, The Android Open Source Project
**
** Licensed under the Apache License, Version 2.0 (the "License");
** you may not use this file except in compliance with the License.
** You may obtain a copy of the License at
**
**     http://www.apache.org/licenses/LICENSE-2.0
**
** Unless required by applicable law or agreed to in writing, software
** distributed under the License is distributed on an "AS IS" BASIS,
** WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
** See the License for the specific language governing permissions and
** limitations under the License.
*/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <limits.h>
#include <errno.h>
#include <time.h>
#include "file.h"
#include "program.h"
#include "mincrypt/sha.h"

#include "bootimg.h"
#include "ramdisk.h"


/*void strrev(char *s) {
  int tmp, i, j;
  for (i = 0, j = strlen(s) - 1; i < j; i++, j--) {
    tmp = s[i];
    s[i] = s[j];
    s[j] = tmp;
  }
}  */ 
#define MAXIMUM_KNOWN_PAGE_SIZE 4096
#define HEADER_PAGE_MASK (header->page_size - 1)
#define KERNEL_SIZE header->kernel_size
#define HEADER_PAGE_SIZE header->page_size
#define KERNEL_PADDING (header->page_size - (header->kernel_size & HEADER_PAGE_MASK))
#define RAMDISK_START HEADER_PAGE_SIZE + KERNEL_SIZE + KERNEL_PADDING
static unsigned char padding[MAXIMUM_KNOWN_PAGE_SIZE] = { 0, };
boot_img_hdr* load_boot_image_header(FILE *fp){
	
	if(!fp){
		fprintf(stderr,"error opening file %s errno:%d %s\n",option_values.image_filename,errno,strerror(errno));
		exit(1);
	}
	fseek(fp,0,SEEK_END);
	unsigned long  size = ftell(fp);
	fseek(fp,0,SEEK_SET);
	long int position = ftell(fp);
	if(size<sizeof(boot_img_hdr)){
		fprintf(stderr,"unexpected filesize %ld\n",(long int)size);
		fclose(fp);
		exit(0);
	}
		
	// allocate a known max page
	byte_p header_buffer=malloc(MAXIMUM_KNOWN_PAGE_SIZE);
	size_t bytes_read = fread(header_buffer,1,MAXIMUM_KNOWN_PAGE_SIZE,fp);
	//fprintf(stderr,"bytes_read %ld\n",(long int)bytes_read);
	byte_p offset=find_in_memory(header_buffer,MAXIMUM_KNOWN_PAGE_SIZE,BOOT_MAGIC,BOOT_MAGIC_SIZE);
	if(!offset) {
		// offset not found in the first double page read the full file to check 
		header_buffer=realloc(header_buffer,size);
		fseek(fp,0,SEEK_SET);
		if((bytes_read = fread(header_buffer,1,size,fp)) != size){
			fprintf(stderr,"bytes_read  %ld size %ld\n",(long int)bytes_read,size);
			fclose(fp);
			exit(0);
		}
		fprintf(stderr,"bytes_read  %ld size %ld\n",(long int)bytes_read,size);
		if ((offset=find_in_memory(header_buffer,size,BOOT_MAGIC,BOOT_MAGIC_SIZE)) == NULL){
			fprintf(stderr,"cannot find android boot image magic\n");
			fclose(fp);
			exit(0);
		}
	}
	fseek(fp,offset-header_buffer,SEEK_SET);
	return (boot_img_hdr*)offset;
}
int get_content_hash(boot_img_hdr* header,unsigned char *kernel_data,unsigned char *ramdisk_data,unsigned char *second_data)
{
	SHA_CTX ctx;
    SHA_init(&ctx);
    SHA_update(&ctx, kernel_data, header->kernel_size);
    SHA_update(&ctx, &header->kernel_size, sizeof(header->kernel_size));
    SHA_update(&ctx, ramdisk_data, header->ramdisk_size);
    SHA_update(&ctx, &header->ramdisk_size, sizeof(header->ramdisk_size));
    SHA_update(&ctx, second_data, header->second_size);
    SHA_update(&ctx, &header->second_size, sizeof(header->second_size));
    const uint8_t* sha = SHA_final(&ctx);
	memcpy(&header->id, sha, SHA_DIGEST_SIZE > sizeof(header->id) ? sizeof(header->id) : SHA_DIGEST_SIZE);
    return 0;

}
static int write_boot_image(FILE* boot_image_file,boot_img_hdr* header,unsigned char *kernel_data,unsigned char *ramdisk_data,unsigned char *second_data)
{
	unsigned pagemask = header->page_size - 1; 
	size_t header_size = (size_t)sizeof(boot_img_hdr);
	get_content_hash(header,kernel_data,ramdisk_data,second_data);
	size_t ramdisk_padding = header->page_size - (header->ramdisk_size & pagemask);
	size_t header_padding = header->page_size - (header_size & pagemask);
	size_t kernel_padding = header->page_size - (header->kernel_size & pagemask);
	size_t second_padding =  header->page_size - (header->second_size & pagemask);
	log_write("write_boot_image Header P:%d S:%ld T:%ld\n",header_padding,header_size,header_size+header_padding); 
	log_write("write_boot_image Kernel P:%d S:%ld T:%ld\n",kernel_padding,header->kernel_size,kernel_padding+header->kernel_size); 
	//log_write("write_boot_image Kernel P:%d S:%d\n",kernel_padding,header->kernel_size,kernel_padding+header->kernel_size); 
	log_write("write_boot_image Ramdisk P:%d S:%ld T:%ld\n",ramdisk_padding,header->ramdisk_size,ramdisk_padding+header->ramdisk_size); 
	
	if(fwrite(header,1,header_size,boot_image_file) !=  header_size) goto header_fail;
	
    if(fwrite(padding,1,header_padding,boot_image_file) != header_padding) goto kernel_fail;

    if(fwrite(kernel_data,1,  header->kernel_size,boot_image_file) !=  header->kernel_size) goto kernel_fail;
	if(fwrite(padding,1,kernel_padding,boot_image_file) != kernel_padding ) goto kernel_fail;

    if(fwrite(ramdisk_data,1, header->ramdisk_size,boot_image_file) != header->ramdisk_size) goto ramdisk_fail;
    if(fwrite(padding,1,ramdisk_padding,boot_image_file) != ramdisk_padding ) goto ramdisk_fail;
    
    
    if(header->second_size>0) {
        if(fwrite(second_data,1,header->second_size,boot_image_file) != header->second_size) goto second_fail;
        if(fwrite(padding,1,second_padding,boot_image_file) != second_padding ) goto second_fail;
    }
    return 0;
header_fail:
	fprintf(stderr,"HEADER FAIL\n");
	return 0; 
kernel_fail:
	fprintf(stderr,"KERNEL FAIL\n");
	return 0; 
ramdisk_fail:
	fprintf(stderr,"RAM FAIL\n");
	return 0; 
second_fail:
	fprintf(stderr,"SECOND FAIL\n");
	return 0; 
fail:
	fprintf(stderr,"FAIL\n");
	return 0; 
}
// Extract Kernel File
// boot_image_file : Boot Image File. File Position must be at the start of the kernel
// Returns : Sets boot_image_file file position to ramdisk entry
static int process_kernel_section(FILE* boot_image_file,boot_img_hdr* header){
	unsigned pagemask = header->page_size - 1;
	if(option_values.kernel_filename){
		fprintf(stderr," Extracting Kernel %s\n",option_values.kernel_filename);
		byte_p buffer=malloc(header->kernel_size);
		fread(buffer,1,header->kernel_size,boot_image_file);
		write_to_file(buffer,header->kernel_size,option_values.kernel_filename);
		free(buffer);
	}else{
		fseek(boot_image_file,header->kernel_size,SEEK_CUR); 
	}
	fseek(boot_image_file,(header->page_size - (header->kernel_size & pagemask)),SEEK_CUR);
	return 0;
} 
static int process_ramdisk_archive(boot_img_hdr* header,byte_p ramdisk_data){
	if(option_values.ramdisk_archive_filename)	
		write_to_file(ramdisk_data,header->ramdisk_size,option_values.ramdisk_archive_filename);
	
	if(	option_values.ramdisk_cpio_filename || option_values.ramdisk_directory_name || option_values.file_list ){
		
		byte_p uncompressed_ramdisk_data = (byte_p) malloc(MEMORY_BUFFER_SIZE) ;
		unsigned long uncompressed_ramdisk_size =	uncompress_gzip_ramdisk_memory(ramdisk_data,header->ramdisk_size,uncompressed_ramdisk_data,MEMORY_BUFFER_SIZE);
		
		if( option_values.ramdisk_cpio_filename )
			write_to_file(uncompressed_ramdisk_data,uncompressed_ramdisk_size,option_values.ramdisk_cpio_filename);
		
		if( option_values.ramdisk_directory_name )	{
			process_uncompressed_ramdisk(uncompressed_ramdisk_data,uncompressed_ramdisk_size,option_values.ramdisk_directory_name);
		}
		if(option_values.file_list){
			find_file_in_ramdisk_entries(uncompressed_ramdisk_data);
		}
		free(uncompressed_ramdisk_data);
	}
	return 0;
}
static int process_ramdisk_section(FILE* boot_image_file,boot_img_hdr* header){
	byte ramdisk_data[header->ramdisk_size];
	 fprintf(stderr," Extracting Ramdisk %s\n",option_values.ramdisk_directory_name);			
	unsigned pagemask = header->page_size - 1;
	long ramdisk_padding = header->page_size - (header->ramdisk_size & pagemask);
	if(option_values.ramdisk_archive_filename || option_values.ramdisk_directory_name || option_values.ramdisk_cpio_filename){
		fprintf(stderr," Extracting Ramdisk\n");
		memset(ramdisk_data,0,header->ramdisk_size);
		fprintf(stderr," Extracting Ramdisk\n");
		fread(ramdisk_data,1,header->ramdisk_size,boot_image_file);
		fseek(boot_image_file,ramdisk_padding,SEEK_CUR); 
	}else{
		fseek(boot_image_file,header->ramdisk_size+ramdisk_padding,SEEK_CUR); 
		return 0;
	}
	process_ramdisk_archive( header, ramdisk_data);
	
	//if(option_values.ramdisk_archive_filename || option_values.ramdisk_directory_name || option_values.ramdisk_cpio_filename)
		//free(ramdisk_data);
	return 0 ;	
		
}
static int process_header_section(FILE* boot_image_file,boot_img_hdr* header){
	if(option_values.header_filename){
		FILE * header_file = fopen(option_values.header_filename,"wb");
		if(header_file){
			fprintf(header_file,"%u"EOL"0x%08x"EOL"%u"EOL"0x%08x"EOL"%u"EOL"0x%08x"EOL"0x%08x"EOL"%u"EOL"%s"EOL"%s"EOL,
			header->kernel_size,header->kernel_addr,header->ramdisk_size,header->ramdisk_addr,
			header->second_size,header->second_addr,header->tags_addr,header->page_size,header->name,header->cmdline);
			fclose(header_file);
		}
	}
	if(option_values.board_filename){
		if(strlen((char*)header->name)==0){
			fprintf(stderr,"Board Name file not written : name header entry is empty!\n");
		}else{
			FILE * board_file = fopen(option_values.board_filename,"wb");			
			if(board_file){
				fprintf(board_file,"%s\n",header->name);
				fclose(board_file);
			}	
		}
	}
	if(option_values.cmdline_filename){
		if(strlen((char*)header->cmdline)==0){
			fprintf(stderr,"Cmdline file not written : cmdline header entry is empty!\n");
		}else{
			FILE * cmdline_file = fopen(option_values.cmdline_filename,"wb");
			if(cmdline_file){
				fprintf(cmdline_file,"%s\n",header->cmdline);
				fclose(cmdline_file);
			}
		}	
	}
	if(option_values.page_size_filename){
		FILE * pagesize_file = fopen(option_values.page_size_filename,"wb");
		if(pagesize_file){
			fprintf(pagesize_file,"%d\n",header->page_size);
			fclose(pagesize_file);
		}
	}
	fseek(boot_image_file , HEADER_PAGE_SIZE, SEEK_CUR);
	return 0;
}
static int unpack_second_file(FILE* boot_image_file,boot_img_hdr* header){
	unsigned pagemask = header->page_size - 1;
	if((header->second_size > 0) && (option_values.second_filename)){
		fprintf(stderr," Extracting Second %s\n",option_values.second_filename);
		byte_p buffer=malloc(header->second_size);
		fread(buffer,1,header->second_size,boot_image_file);
		write_to_file(buffer,header->second_size,option_values.second_filename);
		free(buffer);
	}else{
		fseek(boot_image_file,header->second_size,SEEK_CUR); 
	}
	fseek(boot_image_file,(header->page_size - (header->second_size & pagemask)),SEEK_CUR);
	return 0;
} 
int unpack_boot_image_file()
{
	FILE* boot_image_file = fopen(option_values.image_filename,"r+b");
	boot_img_hdr* header = load_boot_image_header(boot_image_file);
	long  offset = ftell(boot_image_file);
	long data_size = 0,position =0;
	
	fprintf(stderr," Android Boot Image Found @ %ld\n",offset);
	fseek(boot_image_file , header->page_size, SEEK_CUR);
	//process_kernel_section(boot_image_file,header);
	fprintf(stderr," Position  %ld\n",ftell(boot_image_file));
	
	fprintf(stderr," Position  %ld\n",ftell(boot_image_file));
	unpack_second_file(boot_image_file,header);	
	if(option_values.header_filename){
		FILE * header_file = fopen(option_values.header_filename,"wb");
		if(header_file){
			fprintf(header_file,"%u"EOL"0x%08x"EOL"%u"EOL"0x%08x"EOL"%u"EOL"0x%08x"EOL"0x%08x"EOL"%u"EOL"%s"EOL"%s"EOL,
			header->kernel_size,header->kernel_addr,header->ramdisk_size,header->ramdisk_addr,
			header->second_size,header->second_addr,header->tags_addr,header->page_size,header->name,header->cmdline);
			fclose(header_file);
		}
	}
	fclose(boot_image_file);
	exit(0);	
}

int create_boot_image_file(){
	
	
	char starting_directory[PATH_MAX];
	getcwd(starting_directory,PATH_MAX);
	log_write("create_boot_image_file:kernel:%s:ramdisk_directory:%s image:%s starting_directory=%s\n",option_values.kernel_filename,option_values.ramdisk_directory_name,option_values.image_filename,starting_directory);
	byte_p ramdisk_gzip_data = NULL, kernel_data = NULL ,second_data = NULL,cmdline_data = NULL;
	size_t ramdisk_gzip_size = 0, kernel_size=0, second_size = 0;
	unsigned pagemask = option_values.page_size - 1; 
	if ( (option_values.ramdisk_directory_name) || (option_values.ramdisk_cpio_filename ) ){
		
		log_write("create_boot_image_file:packing_ramdisk_directory\n");
		size_t cpio_size =0 ; 
		unsigned char* ramdisk_cpio_data =NULL;
		
		if (option_values.ramdisk_directory_name) {
			ramdisk_cpio_data = calloc(MEMORY_BUFFER_SIZE, sizeof(unsigned char)) ;
			cpio_size = pack_ramdisk_directory(ramdisk_cpio_data); 
		
		}else if ( option_values.ramdisk_cpio_filename  ){
			ramdisk_cpio_data = load_file( option_values.ramdisk_cpio_filename ,&cpio_size);
		
		ramdisk_gzip_data = calloc(cpio_size, sizeof(unsigned char));
		ramdisk_gzip_size = compress_gzip_ramdisk_memory(ramdisk_cpio_data,cpio_size,ramdisk_gzip_data,cpio_size);
		log_write("create_boot_image_file:ramdisk_gzip_size=%lu\n",ramdisk_gzip_size);
		free(ramdisk_cpio_data);
		chdir(starting_directory);
		}
	}
	if ( option_values.ramdisk_archive_filename )	ramdisk_gzip_data = load_file( option_values.ramdisk_archive_filename,&ramdisk_gzip_size );

	if ( option_values.kernel_filename ) kernel_data=load_file(option_values.kernel_filename,&kernel_size);
	if ( option_values.second_filename ) second_data=load_file(option_values.second_filename,&second_size);

	boot_img_hdr header;
	memset(&header, 0, sizeof(header));
	strncpy((char*)header.magic,BOOT_MAGIC,BOOT_MAGIC_SIZE) ;
	
	header.kernel_size = kernel_size;
	header.kernel_addr = DEFAULT_KERNEL_ADDRESS;
	header.ramdisk_size = ramdisk_gzip_size;
	header.ramdisk_addr = DEFAULT_RAMDISK_ADDRESS;
	header.second_size = second_size;
	header.second_addr = DEFAULT_SECOND_ADDRESS;
	header.tags_addr = DEFAULT_TAGS_ADDRESS;
	header.page_size = option_values.page_size;	
	log_write("create_boot_image_file:kernel_size %d\n",kernel_size);
	strncpy((char*)header.cmdline,option_values.cmdline_text,BOOT_ARGS_SIZE);
	strncpy((char*)header.name,option_values.board_name,BOOT_NAME_SIZE);	
	FILE*fp = fopen(option_values.image_filename,"wb");
	write_boot_image(fp,&header,kernel_data,ramdisk_gzip_data,second_data);
    fclose(fp);
    log_write("create_boot_image_file:ramdisk_gzip_size=%lu\n",ramdisk_gzip_size);
    if(second_size) free(second_data);
    free(kernel_data);
	free(ramdisk_gzip_data);
	return 0;
}

int extract_boot_image_file(){
	
	log_write("Extracting %s\n",option_values.image_filename);
	FILE* boot_image_file = fopen(option_values.image_filename,"r+b");
	boot_img_hdr* header = load_boot_image_header(boot_image_file);
	
	process_header_section(boot_image_file,header);
	process_kernel_section(boot_image_file,header);
	process_ramdisk_section(boot_image_file,header);
	log_write("Extracting %s\n",option_values.image_filename);
	
	fclose(boot_image_file);
	
	//byte_p uncompressed_ramdisk_data = (byte_p) malloc(MEMORY_BUFFER_SIZE) ;
	//unsigned long uncompressed_ramdisk_size =	uncompress_gzip_ramdisk_memory(ramdisk_data,header->ramdisk_size,uncompressed_ramdisk_data,MEMORY_BUFFER_SIZE);
	
	
	////fprintf(stderr,"second_filename=%s  strlen=%d ,count=%d\n",option_values.source_filename,strlen(option_values.source_filename),option_values.source_length);
	//free(ramdisk_data);
	
	//find_file_in_ramdisk_entries(uncompressed_ramdisk_data);
	//free(uncompressed_ramdisk_data);
	
	
	return 0;
}

int list_boot_image_info(){
	
	FILE* fp = fopen(option_values.image_filename,"r+b");
	boot_img_hdr* header = load_boot_image_header(fp); 
	long int position = ftell(fp);
	fprintf(stderr," Android Boot Image Found @ %ld\n",position);
	fprintf(stderr," File Header   :\n");
	fprintf(stderr," Kernel Size   : 0x%08x %u\n",header->kernel_size,header->kernel_size);
	fprintf(stderr," Kernel Addr   : 0x%08x %u\n",header->kernel_addr,header->kernel_addr);
	fprintf(stderr," Ramdisk Size  : 0x%08x %u\n",header->ramdisk_size,header->ramdisk_size);
	fprintf(stderr," Ramdisk Addr  : 0x%08x %u\n",header->ramdisk_addr,header->ramdisk_addr);
	fprintf(stderr," Second Size   : 0x%08x %u\n",header->second_size,header->second_size);
	fprintf(stderr," Second Addr   : 0x%08x %u\n",header->second_addr,header->second_addr);
	fprintf(stderr," Tags Addr     : 0x%08x %u\n",header->tags_addr,header->tags_addr);
	fprintf(stderr," Page Size     : 0x%08x %u\n",header->page_size,header->page_size);
	fprintf(stderr," name          : %s\n",header->name);
	fprintf(stderr," cmdline       : %s\n",header->cmdline);
	fclose(fp);
	return 0;
}
// 
byte_p pick_kernel_data(FILE*boot_image_file, const boot_img_hdr* header,boot_img_hdr* new_header, int *rewrite){
	byte_p kernel_data=NULL;
	unsigned pagemask = header->page_size - 1;
	if(option_values.kernel_filename){
		// Using external kernel file. Move file pointer to ramdisk - before we adjust the header
		// Style Note: Probably should use a different variable for the new header values
		// to save confusion

		fprintf(stderr,"Updating boot image %s kernel with %s\n",option_values.image_filename,option_values.kernel_filename);	 
		size_t kernel_size=0;
		kernel_data = load_file(option_values.kernel_filename,	&kernel_size);
		if(memcmp(kernel_data+magic_linux_zimage_offset,magic_linux_zimage,4)){
			fprintf(stderr,"%s is not a valid kernel zImage\n",option_values.kernel_filename);
			fclose(boot_image_file);
			exit(0);
		};
		new_header->kernel_size=kernel_size;
		fseek(boot_image_file,header->kernel_size,SEEK_CUR); 
		(*rewrite)=1;
	}else{ //not updating the kernel . read the original
		kernel_data=malloc(header->kernel_size);
		fread(kernel_data,1,header->kernel_size,boot_image_file);
		
	}
	fseek(boot_image_file,(header->page_size - (header->kernel_size & pagemask)),SEEK_CUR);
	return kernel_data;
}

int update_boot_image_file(){
			
	
	FILE* boot_image_file = fopen(option_values.image_filename,"r+b");
	boot_img_hdr* header = load_boot_image_header(boot_image_file);
	boot_img_hdr new_header ;
	memcpy(&new_header,header,sizeof(boot_img_hdr));
	unsigned pagemask = header->page_size - 1;
	fseek(boot_image_file , header->page_size, SEEK_CUR);
	byte_p ramdisk_gzip_data=NULL , kernel_data=NULL ,second_data=NULL; int rewrite=0;
	kernel_data = pick_kernel_data(boot_image_file,header,&new_header,&rewrite);
	fprintf(stderr,"Kernel ramdisk in boot image %s\n",option_values.image_filename);
	
	if(option_values.source_filename){ //updating ramdisk 
		fseek(boot_image_file,header->ramdisk_size+(header->page_size - (header->ramdisk_size & pagemask)),SEEK_CUR);
		fprintf(stderr,"Updating ramdisk in boot image %s\n",option_values.image_filename);
		byte_p ramdisk_data =malloc(header->ramdisk_size);
		fread(ramdisk_data,1,header->ramdisk_size,boot_image_file);
		byte_p uncompressed_ramdisk_data = (byte_p) malloc(MEMORY_BUFFER_SIZE) ;
		size_t uncompressed_ramdisk_size =	uncompress_gzip_ramdisk_memory(ramdisk_data,header->ramdisk_size,uncompressed_ramdisk_data,MEMORY_BUFFER_SIZE);
		//log_write("modify_size:%ld\n",uncompressed_ramdisk_size);
		size_t new_cpio_file_size=uncompressed_ramdisk_size;
		
		char* str_p = option_values.source_filename;
		byte_p new_cpio_data = uncompressed_ramdisk_data ; 
		int counter=0;
		for(counter=0 ; counter<=option_values.file_list_count; counter++){	
			option_values.target_filename = option_values.source_filename=option_values.file_list;
			fprintf(stderr,"mod: %s %s\n", option_values.source_filename,option_values.target_filename );
			new_cpio_data =modify_ramdisk_entry(new_cpio_data,new_cpio_file_size,&new_cpio_file_size);
			option_values.file_list +=strlen(option_values.file_list	)+1 ;
		}
		
		
		free(uncompressed_ramdisk_data);
		free(ramdisk_data);
		// nothing done
		if(new_cpio_data!=uncompressed_ramdisk_data){
			
			ramdisk_gzip_data = calloc(new_cpio_file_size, sizeof(unsigned char));
			size_t ramdisk_gzip_size = compress_gzip_ramdisk_memory(new_cpio_data,new_cpio_file_size,ramdisk_gzip_data,new_cpio_file_size);
			free(new_cpio_data);	
			new_header.ramdisk_size = ramdisk_gzip_size;
			rewrite=1;
		}
	}else {
		fprintf(stderr,"Using Existing  ramdisk in boot image %s %u\n",option_values.image_filename,header->ramdisk_size);
		ramdisk_gzip_data=malloc(header->ramdisk_size);
		fread(ramdisk_gzip_data,1,header->ramdisk_size,boot_image_file);
	}
	fseek(boot_image_file,(header->page_size - (header->ramdisk_size & pagemask)),SEEK_CUR);
	second_data=malloc(header->second_size);
	fread(second_data,1,header->second_size,boot_image_file);
	fseek(boot_image_file,(header->page_size - (header->second_size & pagemask)),SEEK_CUR);
	if(rewrite){
		if(option_values.board_name)strncpy((char*)header->name,option_values.board_name,BOOT_NAME_SIZE) ;
		if (option_values.cmdline_text)strncpy((char*)header->cmdline,option_values.cmdline_text,BOOT_ARGS_SIZE);
		fclose(boot_image_file);
		FILE*fp = fopen(option_values.image_filename,"wb");
		write_boot_image(fp,&new_header,kernel_data,ramdisk_gzip_data,second_data);
		fclose(fp);
		
		free(kernel_data);
		free(ramdisk_gzip_data);
		free(second_data);
	}else{ 
		if(option_values.board_name){
			fseek(boot_image_file,header->name-(unsigned char*)header,SEEK_SET);
			fwrite(option_values.board_name,1,BOOT_NAME_SIZE,boot_image_file);	
		}
		if (option_values.cmdline_text){
			fseek(boot_image_file,header->cmdline-(unsigned char*)header,SEEK_SET);
			fwrite(option_values.cmdline_text,1,BOOT_ARGS_SIZE,boot_image_file);	
		}
		fclose(boot_image_file);
	}
	
	
	return 0;
	exit(0);
}
