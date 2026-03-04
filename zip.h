namespace KERF_NAMESPACE {

struct ZIP_ALGO
{
  void *parent = nullptr;
  virtual I   compress(char* src, I src_size, char* dest, I dest_capacity) {return 0;}
  virtual I decompress(char* src, I src_size, char* dest, I dest_capacity) {return 0;}
  I go(char* src, I src_size, char* dest, I dest_capacity, bool decompress_if_true = false)
  {
    if(!decompress_if_true) return compress(src, src_size, dest, dest_capacity);
    return decompress(src, src_size, dest, dest_capacity);
  }
};

struct ZIP_XFORM
{
  // either transform OR both fwd and reverse need to be implemented
  virtual void transform(char* src, I src_size, char* dest, I chunk_size_log_bytes, const bool reverse_if_true = false)
  {
    if(!reverse_if_true) transform_forward(src, src_size, dest, chunk_size_log_bytes);
    else transform_reverse(src, src_size, dest, chunk_size_log_bytes);
  }
  virtual void transform_forward(char* src, I src_size, char* dest, I chunk_size_log_bytes) {transform(src, src_size, dest, chunk_size_log_bytes, false);}
  virtual void transform_reverse(char* src, I src_size, char* dest, I chunk_size_log_bytes) {transform(src, src_size, dest, chunk_size_log_bytes, true);}
};

struct ZIP_XFORM_IDENTITY : ZIP_XFORM
{
  void transform(char* src, I src_size, char* dest, I chunk_size_log_bytes, const bool reverse_if_true) { }
};

struct ZIP_XFORM_DELTA_VAR_BYTE_GROUPING : ZIP_XFORM
{
  void transform(char* src, I src_size, char* dest, I chunk_size_log_bytes, const bool reverse_if_true = false)
  {
    assert(src_size >= POW2(chunk_size_log_bytes));
    assert((dest < src && dest + src_size <= src) || (src < dest && src + src_size <= dest));

    I0 *i0 = (I0*)src, *j0 = (I0*)dest, *k0 = i0; 
    I1 *i1 = (I1*)src, *j1 = (I1*)dest, *k1 = i1; 
    I2 *i2 = (I2*)src, *j2 = (I2*)dest, *k2 = i2; 
    I3 *i3 = (I3*)src, *j3 = (I3*)dest, *k3 = i3; 

    I n = src_size >> chunk_size_log_bytes;

    I m = 1;

    if(reverse_if_true)
    {
      k0 = j0;
      k1 = j1;
      k2 = j2;
      k3 = j3;
      m = -1;
    }

    switch(chunk_size_log_bytes)
    {
      case 0: *j0 = *i0; DO(n - 1, j0[i+1] = i0[i+1] + m * k0[i]) break;
      case 1: *j1 = *i1; DO(n - 1, j1[i+1] = i1[i+1] + m * k1[i]) break;
      case 2: *j2 = *i2; DO(n - 1, j2[i+1] = i2[i+1] + m * k2[i]) break;
      default:
      case 3: *j3 = *i3; DO(n - 1, j3[i+1] = i3[i+1] + m * k3[i]) break;
    }
  }
};

struct ZIP_XFORM_XOR_VAR_BYTE_GROUPING : ZIP_XFORM
{
  void transform(char* src, I src_size, char* dest, I chunk_size_log_bytes, const bool reverse_if_true = false)
  {
    assert(src_size >= POW2(chunk_size_log_bytes));
    assert((dest < src && dest + src_size <= src) || (src < dest && src + src_size <= dest));

    UI0 *i0 = (UI0*)src, *j0 = (UI0*)dest, *k0 = i0; 
    UI1 *i1 = (UI1*)src, *j1 = (UI1*)dest, *k1 = i1; 
    UI2 *i2 = (UI2*)src, *j2 = (UI2*)dest, *k2 = i2; 
    UI3 *i3 = (UI3*)src, *j3 = (UI3*)dest, *k3 = i3; 

    I n = src_size >> chunk_size_log_bytes;

    if(reverse_if_true)
    {
      k0 = j0;
      k1 = j1;
      k2 = j2;
      k3 = j3;
    }

    switch(chunk_size_log_bytes)
    {
      case 0: *j0 = *i0; DO(n - 1, j0[i+1] = i0[i+1] ^ k0[i]) break;
      case 1: *j1 = *i1; DO(n - 1, j1[i+1] = i1[i+1] ^ k1[i]) break;
      case 2: *j2 = *i2; DO(n - 1, j2[i+1] = i2[i+1] ^ k2[i]) break;
      default:                                              
      case 3: *j3 = *i3; DO(n - 1, j3[i+1] = i3[i+1] ^ k3[i]) break;
    }
  }
};

struct ZIP_ALGO_IDENTITY : ZIP_ALGO
{
  I compress(char* src, I src_size, char* dest, I dest_capacity)
  {
    assert(dest_capacity >= src_size);
    memcpy(dest, src, src_size);
    return src_size;
  }

  I decompress(char* src, I src_size, char* dest, I dest_capacity) {return compress(src,src_size,dest,dest_capacity);}
};

struct ZIP_ALGO_LZ4 : ZIP_ALGO 
{
  // 2022.06.09 one reason not to try to goad lz4 into compressing "in-place" is because incompressible data can cause the compressed data cursor to outrun the uncompressed data cursor. Maybe there is a workaround for this under certain conditions (eg max zip-page size). I guess it's a POP to try to compress in-place, but, it's going to require digging into the guts of lz4 (and may not work). At first glance, it also needs asan suppression in 1.9.3. IMO it's not worth the trouble.

  I compress(char* src, I src_size, char* dest, I dest_capacity)
  {
    static_assert(sizeof(*this) <= sizeof(ZIP_ALGO));
    assert((dest < src && dest + dest_capacity <= src) || (src < dest && src + src_size <= dest));
    assert(dest_capacity >= 1.25*src_size); // or whatever guarantee you need

    int acceleration = 1; // this is where we could hook into parent to retrieve custom values
    if(parent != nullptr)
    {
      // get map attribute
      // if map attribute exists
      {
        acceleration = 2; // populate from map attribute
      }
    }

    I wrote = LZ4_compress_fast(src, dest, src_size, dest_capacity, acceleration);
    if(wrote <= 0)
    {
      fprintf(stderr, "LZ4 compression error: %lld\n", wrote);
    }
    return wrote;
  }

  I decompress(char* src, I src_size, char* dest, I dest_capacity)
  {
    assert((dest < src && dest + dest_capacity <= src) || (src < dest && src + src_size <= dest));
    I wrote = LZ4_decompress_safe(src, dest, src_size, dest_capacity);

    if(wrote <=0)
    {
      fprintf(stderr, "LZ4 decompression error: %lld\n", wrote);
    }

    return wrote;
  }
};

struct ZIP_ALGO_ZSTD : ZIP_ALGO 
{
  I compress(char* src, I src_size, char* dest, I dest_capacity)
  {
    static_assert(sizeof(*this) <= sizeof(ZIP_ALGO));
    assert((dest < src && dest + dest_capacity <= src) || (src < dest && src + src_size <= dest));
    assert(dest_capacity >= 1.25*src_size); // or whatever guarantee you need

    // this is where we could hook into parent to retrieve custom values
    if(parent != nullptr)
    {
      // get map attribute
      // if map attribute exists
      {
        // populate from map attribute
      }
    }

    die(zstd compress not yet implemented);

    I wrote = 0;

    return wrote;
  }

  I decompress(char* src, I src_size, char* dest, I dest_capacity)
  {
    assert((dest < src && dest + dest_capacity <= src) || (src < dest && src + src_size <= dest));
    die(zstd decompress not yet implemented);
    I wrote = 0;

    return wrote;
  }
};


void init_algo(void *space, ZIP_ALGORITHM e)
{
  // HACK - slicing is a potential if `space` has less room than width of max-sized ZIP_ALGO derived object
  switch(e)
  {
    case ZIP_ALGORITHM_IDENTITY: new (space) ZIP_ALGO_IDENTITY(); break;
    case ZIP_ALGORITHM_LZ4_1:    new (space) ZIP_ALGO_LZ4();      break;
    case ZIP_ALGORITHM_ZSTD_1:   new (space) ZIP_ALGO_ZSTD();     break;
    default:
      die(zip algo not yet implemented);
  }


}


} // namespace
