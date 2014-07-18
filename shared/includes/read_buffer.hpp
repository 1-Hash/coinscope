#ifndef READ_BUFFER_HPP
#define READ_BUFFER_HPP

#include "mmap_buffer.hpp"

/* to be used for accumulating read calls and extract mmap_buffers when ready to act on the data */
class read_buffer { 
public:
	/* return value from read, whether the read is complete (i.e., buffer can be extracted) */
	read_buffer(size_t to_read) : cursor_(0), to_read_(to_read), buffer_(to_read_) {}
	std::pair<int,bool> do_read(int fd); /* will read to_read_ bytes */
	std::pair<int,bool> do_read(int fd, size_t size); /* will read size bytes */
   void to_read(size_t);
   size_t to_read() const;
	void cursor(size_t loc) ;
	size_t cursor() const;
	bool hungry() const;
	mmap_buffer<uint8_t> extract_buffer();
private:
	size_t cursor_;
	size_t to_read_;
	mmap_buffer<uint8_t> buffer_;

};

#endif
